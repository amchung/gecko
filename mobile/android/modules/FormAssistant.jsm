/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const {classes: Cc, interfaces: Ci, utils: Cu} = Components;

this.EXPORTED_SYMBOLS = ["FormAssistant"];

Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyModuleGetters(this, {
  FormHistory: "resource://gre/modules/FormHistory.jsm",
  GeckoViewUtils: "resource://gre/modules/GeckoViewUtils.jsm",
  Services: "resource://gre/modules/Services.jsm",
});

var FormAssistant = {
  // Weak-ref used to keep track of the currently focused element.
  _currentFocusedElement: null,

  // Whether we're in the middle of an autocomplete.
  _doingAutocomplete: false,

  init: function() {
    Services.obs.addObserver(this, "PanZoom:StateChange");
  },

  _onPopupResponse: function(currentElement, message) {
    switch (message.action) {
      case "autocomplete": {
        let editableElement = currentElement.QueryInterface(Ci.nsIDOMNSEditableElement);
        this._doingAutocomplete = true;

        // If we have an active composition string, commit it before sending
        // the autocomplete event with the text that will replace it.
        try {
          if (editableElement.editor.composing) {
            editableElement.editor.forceCompositionEnd();
          }
        } catch (e) {}

        editableElement.setUserInput(message.value);

        let event = currentElement.ownerDocument.createEvent("Events");
        event.initEvent("DOMAutoComplete", true, true);
        currentElement.dispatchEvent(event);

        this._doingAutocomplete = false;
        break;
      }

      case "remove": {
        FormHistory.update({
          op: "remove",
          fieldname: currentElement.name,
          value: message.value,
        });
        break;
      }
    }
  },

  observe: function(aSubject, aTopic, aData) {
    switch (aTopic) {
      case "PanZoom:StateChange":
        // If the user is just touching the screen and we haven't entered a pan
        // or zoom state yet do nothing.
        if (aData == "TOUCHING" || aData == "WAITING_LISTENERS") {
          break;
        }
        let focused = this._currentFocusedElement && this._currentFocusedElement.get();
        if (aData == "NOTHING") {
          if (!focused || this._showValidationMessage(focused)) {
            break;
          }
          this._showAutoCompleteSuggestions(focused, hasResults => {
            if (!hasResults) {
              this._hideFormAssistPopup(focused);
            }
          });
        } else if (focused) {
          // temporarily hide the form assist popup while we're panning or zooming the page
          this._hideFormAssistPopup(focused);
        }
        break;
    }
  },

  notifyInvalidSubmit: function(aFormElement, aInvalidElements) {
    if (!aInvalidElements.length) {
        return;
    }

    // Ignore this notificaiton if the current tab doesn't contain the invalid element
    let currentElement = aInvalidElements.queryElementAt(0, Ci.nsISupports);
    let focused = this._currentFocusedElement && this._currentFocusedElement.get();
    if (focused && focused.ownerGlobal.top !== currentElement.ownerGlobal.top) {
        return;
    }

    // Our focus listener will show the element's validation message
    currentElement.focus();
  },

  handleEvent: function(aEvent) {
    switch (aEvent.type) {
      case "focus": {
        let currentElement = aEvent.target;
        // Only show a validation message on focus.
        if (this._showValidationMessage(currentElement) ||
            this._isAutoComplete(currentElement)) {
          this._currentFocusedElement = Cu.getWeakReference(currentElement);
        }
        break;
      }

      case "blur": {
        let focused = this._currentFocusedElement && this._currentFocusedElement.get();
        if (focused) {
          this._hideFormAssistPopup(focused);
        }
        this._currentFocusedElement = null;
        break;
      }

      case "click": {
        let currentElement = aEvent.target;

        // Prioritize a form validation message over autocomplete suggestions
        // when the element is first focused (a form validation message will
        // only be available if an invalid form was submitted)
        if (this._showValidationMessage(currentElement)) {
          break;
        }

        let checkResultsClick = hasResults => {
          if (!hasResults) {
            this._hideFormAssistPopup(currentElement);
          }
        };

        this._showAutoCompleteSuggestions(currentElement, checkResultsClick);
        break;
      }

      case "input": {
        let currentElement = aEvent.target;
        let focused = this._currentFocusedElement && this._currentFocusedElement.get();

        // If this element isn't focused, we're already in middle of an
        // autocomplete, or its value hasn't changed, don't show the
        // autocomplete popup.
        if (currentElement !== focused || this._doingAutocomplete) {
          break;
        }

        // Since we can only show one popup at a time, prioritze autocomplete
        // suggestions over a form validation message
        let checkResultsInput = hasResults => {
          if (hasResults || this._showValidationMessage(currentElement)) {
            return;
          }
          // If we're not showing autocomplete suggestions, hide the form assist popup
          this._hideFormAssistPopup(currentElement);
        };

        this._showAutoCompleteSuggestions(currentElement, checkResultsInput);
        break;
      }
    }
  },

  // We only want to show autocomplete suggestions for certain elements
  _isAutoComplete: function(aElement) {
    return (aElement instanceof Ci.nsIDOMHTMLInputElement) &&
           !aElement.readOnly &&
           !this._isDisabledElement(aElement) &&
           (aElement.type !== "password") &&
           (aElement.autocomplete !== "off");
  },

  // Retrieves autocomplete suggestions for an element from the form autocomplete service.
  // aCallback(array_of_suggestions) is called when results are available.
  _getAutoCompleteSuggestions: function(aSearchString, aElement, aCallback) {
    // Cache the form autocomplete service for future use
    if (!this._formAutoCompleteService) {
      this._formAutoCompleteService = Cc["@mozilla.org/satchel/form-autocomplete;1"]
          .getService(Ci.nsIFormAutoComplete);
    }

    let resultsAvailable = function(results) {
      let suggestions = [];
      for (let i = 0; i < results.matchCount; i++) {
        let value = results.getValueAt(i);

        // Do not show the value if it is the current one in the input field
        if (value == aSearchString)
          continue;

        // Supply a label and value, since they can differ for datalist suggestions
        suggestions.push({ label: value, value: value });
      }
      aCallback(suggestions);
    };

    this._formAutoCompleteService.autoCompleteSearchAsync(aElement.name || aElement.id,
                                                          aSearchString, aElement, null,
                                                          null, resultsAvailable);
  },

  /**
   * (Copied from mobile/xul/chrome/content/forms.js)
   * This function is similar to getListSuggestions from
   * components/satchel/src/nsInputListAutoComplete.js but sadly this one is
   * used by the autocomplete.xml binding which is not in used in fennec
   */
  _getListSuggestions: function(aElement) {
    if (!(aElement instanceof Ci.nsIDOMHTMLInputElement) || !aElement.list) {
      return [];
    }

    let suggestions = [];
    let filter = !aElement.hasAttribute("mozNoFilter");
    let lowerFieldValue = aElement.value.toLowerCase();

    let options = aElement.list.options;
    let length = options.length;
    for (let i = 0; i < length; i++) {
      let item = options.item(i);

      let label = item.value;
      if (item.label) {
        label = item.label;
      } else if (item.text) {
        label = item.text;
      }

      if (filter && !(label.toLowerCase().includes(lowerFieldValue))) {
        continue;
      }
      suggestions.push({ label: label, value: item.value });
    }

    return suggestions;
  },

  // Retrieves autocomplete suggestions for an element from the form autocomplete service
  // and sends the suggestions to the Java UI, along with element position data. As
  // autocomplete queries are asynchronous, calls aCallback when done with a true
  // argument if results were found and false if no results were found.
  _showAutoCompleteSuggestions: function(aElement, aCallback) {
    if (!this._isAutoComplete(aElement)) {
      aCallback(false);
      return;
    }

    let isEmpty = (aElement.value.length === 0);

    let resultsAvailable = autoCompleteSuggestions => {
      // On desktop, we show datalist suggestions below autocomplete suggestions,
      // without duplicates removed.
      let listSuggestions = this._getListSuggestions(aElement);
      let suggestions = autoCompleteSuggestions.concat(listSuggestions);

      // Return false if there are no suggestions to show
      if (!suggestions.length) {
        aCallback(false);
        return;
      }

      GeckoViewUtils.getDispatcherForWindow(aElement.ownerGlobal).sendRequest({
        type: "FormAssist:AutoCompleteResult",
        suggestions: suggestions,
        rect: this._getBoundingContentRect(aElement),
        isEmpty: isEmpty,
      }, {
        onSuccess: response => this._onPopupResponse(aElement, response),
        onError: error => Cu.reportError(error),
      });

      aCallback(true);
    };

    this._getAutoCompleteSuggestions(aElement.value, aElement, resultsAvailable);
  },

  // Only show a validation message if the user submitted an invalid form,
  // there's a non-empty message string, and the element is the correct type
  _isValidateable: function(aElement) {
    return (aElement instanceof Ci.nsIDOMHTMLInputElement ||
            aElement instanceof Ci.nsIDOMHTMLTextAreaElement ||
            aElement instanceof Ci.nsIDOMHTMLSelectElement ||
            aElement instanceof Ci.nsIDOMHTMLButtonElement) &&
           aElement.validationMessage;
  },

  // Sends a validation message and position data for an element to the Java UI.
  // Returns true if there's a validation message to show, false otherwise.
  _showValidationMessage: function(aElement) {
    if (!this._isValidateable(aElement)) {
      return false;
    }

    GeckoViewUtils.getDispatcherForWindow(aElement.ownerGlobal).sendRequest({
      type: "FormAssist:ValidationMessage",
      validationMessage: aElement.validationMessage,
      rect: this._getBoundingContentRect(aElement),
    });
    return true;
  },

  _hideFormAssistPopup: function(aElement) {
    if (!aElement.ownerGlobal) {
      return;
    }
    GeckoViewUtils.getDispatcherForWindow(aElement.ownerGlobal).sendRequest({
      type: "FormAssist:Hide",
    });
  },

  _isDisabledElement: function(aElement) {
    let currentElement = aElement;
    while (currentElement) {
      if (currentElement.disabled) {
        return true;
      }
      currentElement = currentElement.parentElement;
    }
    return false;
  },

  _getBoundingContentRect: function(aElement) {
    if (!aElement) {
      return {x: 0, y: 0, w: 0, h: 0};
    }

    let document = aElement.ownerDocument;
    while (document.defaultView.frameElement) {
      document = document.defaultView.frameElement.ownerDocument;
    }

    let cwu = document.defaultView.QueryInterface(Ci.nsIInterfaceRequestor)
                                  .getInterface(Ci.nsIDOMWindowUtils);
    let scrollX = {}, scrollY = {};
    cwu.getScrollXY(false, scrollX, scrollY);

    let r = aElement.getBoundingClientRect();

    // step out of iframes and frames, offsetting scroll values
    for (let frame = aElement.ownerGlobal; frame.frameElement && frame != content;
         frame = frame.parent) {
      // adjust client coordinates' origin to be top left of iframe viewport
      let rect = frame.frameElement.getBoundingClientRect();
      let left = frame.getComputedStyle(frame.frameElement).borderLeftWidth;
      let top = frame.getComputedStyle(frame.frameElement).borderTopWidth;
      scrollX.value += rect.left + parseInt(left);
      scrollY.value += rect.top + parseInt(top);
    }

    return {
      x: r.left + scrollX.value,
      y: r.top + scrollY.value,
      w: r.width,
      h: r.height,
    };
  },
};

FormAssistant.init();
