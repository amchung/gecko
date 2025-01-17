/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Smart pointers for the JS-managed DOM objects.
//!
//! The DOM is made up of DOM objects whose lifetime is entirely controlled by
//! the whims of the SpiderMonkey garbage collector. The types in this module
//! are designed to ensure that any interactions with said Rust types only
//! occur on values that will remain alive the entire time.
//!
//! Here is a brief overview of the important types:
//!
//! - `Root<T>`: a stack-based reference to a rooted DOM object.
//! - `JS<T>`: a reference to a DOM object that can automatically be traced by
//!   the GC when encountered as a field of a Rust structure.
//!
//! `JS<T>` does not allow access to their inner value without explicitly
//! creating a stack-based root via the `root` method. This returns a `Root<T>`,
//! which causes the JS-owned value to be uncollectable for the duration of the
//! `Root` object's lifetime. A reference to the object can then be obtained
//! from the `Root` object. These references are not allowed to outlive their
//! originating `Root<T>`.
//!

use core::nonzero::NonZero;
use dom::bindings::conversions::DerivedFrom;
use dom::bindings::inheritance::Castable;
use dom::bindings::reflector::{DomObject, Reflector};
use dom::bindings::trace::JSTraceable;
use dom::bindings::trace::trace_reflector;
use dom::node::Node;
use heapsize::HeapSizeOf;
use js::jsapi::{JSObject, JSTracer, Heap};
use js::rust::GCMethods;
use mitochondria::OnceCell;
use script_layout_interface::TrustedNodeAddress;
use script_thread::STACK_ROOTS;
use std::cell::UnsafeCell;
use std::default::Default;
use std::hash::{Hash, Hasher};
#[cfg(debug_assertions)]
use std::intrinsics::type_name;
use std::mem;
use std::ops::Deref;
use std::ptr;
use std::rc::Rc;
use style::thread_state;

/// A traced reference to a DOM object
///
/// This type is critical to making garbage collection work with the DOM,
/// but it is very dangerous; if garbage collection happens with a `JS<T>`
/// on the stack, the `JS<T>` can point to freed memory.
///
/// This should only be used as a field in other DOM objects.
#[must_root]
pub struct JS<T> {
    ptr: NonZero<*const T>,
}

// JS<T> is similar to Rc<T>, in that it's not always clear how to avoid double-counting.
// For now, we choose not to follow any such pointers.
impl<T> HeapSizeOf for JS<T> {
    fn heap_size_of_children(&self) -> usize {
        0
    }
}

impl<T> JS<T> {
    /// Returns `LayoutJS<T>` containing the same pointer.
    pub unsafe fn to_layout(&self) -> LayoutJS<T> {
        debug_assert!(thread_state::get().is_layout());
        LayoutJS {
            ptr: self.ptr.clone(),
        }
    }
}

impl<T: DomObject> JS<T> {
    /// Create a JS<T> from a &T
    #[allow(unrooted_must_root)]
    pub fn from_ref(obj: &T) -> JS<T> {
        debug_assert!(thread_state::get().is_script());
        JS {
            ptr: unsafe { NonZero::new_unchecked(&*obj) },
        }
    }
}

impl<'root, T: DomObject + 'root> RootedReference<'root> for JS<T> {
    type Ref = &'root T;
    fn r(&'root self) -> &'root T {
        &self
    }
}

impl<T: DomObject> Deref for JS<T> {
    type Target = T;

    fn deref(&self) -> &T {
        debug_assert!(thread_state::get().is_script());
        // We can only have &JS<T> from a rooted thing, so it's safe to deref
        // it to &T.
        unsafe { &*self.ptr.get() }
    }
}

unsafe impl<T: DomObject> JSTraceable for JS<T> {
    unsafe fn trace(&self, trc: *mut JSTracer) {
        #[cfg(debug_assertions)]
        let trace_str = format!("for {} on heap", type_name::<T>());
        #[cfg(debug_assertions)]
        let trace_info = &trace_str[..];
        #[cfg(not(debug_assertions))]
        let trace_info = "for DOM object on heap";

        trace_reflector(trc,
                        trace_info,
                        (*self.ptr.get()).reflector());
    }
}

/// An unrooted reference to a DOM object for use in layout. `Layout*Helpers`
/// traits must be implemented on this.
#[allow_unrooted_interior]
pub struct LayoutJS<T> {
    ptr: NonZero<*const T>,
}

impl<T: Castable> LayoutJS<T> {
    /// Cast a DOM object root upwards to one of the interfaces it derives from.
    pub fn upcast<U>(&self) -> LayoutJS<U>
        where U: Castable,
              T: DerivedFrom<U>
    {
        debug_assert!(thread_state::get().is_layout());
        let ptr: *const T = self.ptr.get();
        LayoutJS {
            ptr: unsafe { NonZero::new_unchecked(ptr as *const U) },
        }
    }

    /// Cast a DOM object downwards to one of the interfaces it might implement.
    pub fn downcast<U>(&self) -> Option<LayoutJS<U>>
        where U: DerivedFrom<T>
    {
        debug_assert!(thread_state::get().is_layout());
        unsafe {
            if (*self.unsafe_get()).is::<U>() {
                let ptr: *const T = self.ptr.get();
                Some(LayoutJS {
                    ptr: NonZero::new_unchecked(ptr as *const U),
                })
            } else {
                None
            }
        }
    }
}

impl<T: DomObject> LayoutJS<T> {
    /// Get the reflector.
    pub unsafe fn get_jsobject(&self) -> *mut JSObject {
        debug_assert!(thread_state::get().is_layout());
        (*self.ptr.get()).reflector().get_jsobject().get()
    }
}

impl<T> Copy for LayoutJS<T> {}

impl<T> PartialEq for JS<T> {
    fn eq(&self, other: &JS<T>) -> bool {
        self.ptr == other.ptr
    }
}

impl<T> Eq for JS<T> {}

impl<T> PartialEq for LayoutJS<T> {
    fn eq(&self, other: &LayoutJS<T>) -> bool {
        self.ptr == other.ptr
    }
}

impl<T> Eq for LayoutJS<T> {}

impl<T> Hash for JS<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.ptr.hash(state)
    }
}

impl<T> Hash for LayoutJS<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.ptr.hash(state)
    }
}

impl <T> Clone for JS<T> {
    #[inline]
    #[allow(unrooted_must_root)]
    fn clone(&self) -> JS<T> {
        debug_assert!(thread_state::get().is_script());
        JS {
            ptr: self.ptr.clone(),
        }
    }
}

impl <T> Clone for LayoutJS<T> {
    #[inline]
    fn clone(&self) -> LayoutJS<T> {
        debug_assert!(thread_state::get().is_layout());
        LayoutJS {
            ptr: self.ptr.clone(),
        }
    }
}

impl LayoutJS<Node> {
    /// Create a new JS-owned value wrapped from an address known to be a
    /// `Node` pointer.
    pub unsafe fn from_trusted_node_address(inner: TrustedNodeAddress) -> LayoutJS<Node> {
        debug_assert!(thread_state::get().is_layout());
        let TrustedNodeAddress(addr) = inner;
        LayoutJS {
            ptr: NonZero::new_unchecked(addr as *const Node),
        }
    }
}

/// A holder that provides interior mutability for GC-managed values such as
/// `JS<T>`.  Essentially a `Cell<JS<T>>`, but safer.
///
/// This should only be used as a field in other DOM objects; see warning
/// on `JS<T>`.
#[must_root]
#[derive(JSTraceable)]
pub struct MutJS<T: DomObject> {
    val: UnsafeCell<JS<T>>,
}

impl<T: DomObject> MutJS<T> {
    /// Create a new `MutJS`.
    pub fn new(initial: &T) -> MutJS<T> {
        debug_assert!(thread_state::get().is_script());
        MutJS {
            val: UnsafeCell::new(JS::from_ref(initial)),
        }
    }

    /// Set this `MutJS` to the given value.
    pub fn set(&self, val: &T) {
        debug_assert!(thread_state::get().is_script());
        unsafe {
            *self.val.get() = JS::from_ref(val);
        }
    }

    /// Get the value in this `MutJS`.
    pub fn get(&self) -> Root<T> {
        debug_assert!(thread_state::get().is_script());
        unsafe {
            Root::from_ref(&*ptr::read(self.val.get()))
        }
    }
}

impl<T: DomObject> HeapSizeOf for MutJS<T> {
    fn heap_size_of_children(&self) -> usize {
        // See comment on HeapSizeOf for JS<T>.
        0
    }
}

impl<T: DomObject> PartialEq for MutJS<T> {
   fn eq(&self, other: &Self) -> bool {
        unsafe {
            *self.val.get() == *other.val.get()
        }
    }
}

impl<T: DomObject + PartialEq> PartialEq<T> for MutJS<T> {
    fn eq(&self, other: &T) -> bool {
        unsafe {
            **self.val.get() == *other
        }
    }
}

/// A holder that provides interior mutability for GC-managed values such as
/// `JS<T>`, with nullability represented by an enclosing Option wrapper.
/// Essentially a `Cell<Option<JS<T>>>`, but safer.
///
/// This should only be used as a field in other DOM objects; see warning
/// on `JS<T>`.
#[must_root]
#[derive(JSTraceable)]
pub struct MutNullableJS<T: DomObject> {
    ptr: UnsafeCell<Option<JS<T>>>,
}

impl<T: DomObject> MutNullableJS<T> {
    /// Create a new `MutNullableJS`.
    pub fn new(initial: Option<&T>) -> MutNullableJS<T> {
        debug_assert!(thread_state::get().is_script());
        MutNullableJS {
            ptr: UnsafeCell::new(initial.map(JS::from_ref)),
        }
    }

    /// Retrieve a copy of the current inner value. If it is `None`, it is
    /// initialized with the result of `cb` first.
    pub fn or_init<F>(&self, cb: F) -> Root<T>
        where F: FnOnce() -> Root<T>
    {
        debug_assert!(thread_state::get().is_script());
        match self.get() {
            Some(inner) => inner,
            None => {
                let inner = cb();
                self.set(Some(&inner));
                inner
            },
        }
    }

    /// Retrieve a copy of the inner optional `JS<T>` as `LayoutJS<T>`.
    /// For use by layout, which can't use safe types like Temporary.
    #[allow(unrooted_must_root)]
    pub unsafe fn get_inner_as_layout(&self) -> Option<LayoutJS<T>> {
        debug_assert!(thread_state::get().is_layout());
        ptr::read(self.ptr.get()).map(|js| js.to_layout())
    }

    /// Get a rooted value out of this object
    #[allow(unrooted_must_root)]
    pub fn get(&self) -> Option<Root<T>> {
        debug_assert!(thread_state::get().is_script());
        unsafe {
            ptr::read(self.ptr.get()).map(|o| Root::from_ref(&*o))
        }
    }

    /// Set this `MutNullableJS` to the given value.
    pub fn set(&self, val: Option<&T>) {
        debug_assert!(thread_state::get().is_script());
        unsafe {
            *self.ptr.get() = val.map(|p| JS::from_ref(p));
        }
    }

    /// Gets the current value out of this object and sets it to `None`.
    pub fn take(&self) -> Option<Root<T>> {
        let value = self.get();
        self.set(None);
        value
    }
}

impl<T: DomObject> PartialEq for MutNullableJS<T> {
    fn eq(&self, other: &Self) -> bool {
        unsafe {
            *self.ptr.get() == *other.ptr.get()
        }
    }
}

impl<'a, T: DomObject> PartialEq<Option<&'a T>> for MutNullableJS<T> {
    fn eq(&self, other: &Option<&T>) -> bool {
        unsafe {
            *self.ptr.get() == other.map(JS::from_ref)
        }
    }
}

impl<T: DomObject> Default for MutNullableJS<T> {
    #[allow(unrooted_must_root)]
    fn default() -> MutNullableJS<T> {
        debug_assert!(thread_state::get().is_script());
        MutNullableJS {
            ptr: UnsafeCell::new(None),
        }
    }
}

impl<T: DomObject> HeapSizeOf for MutNullableJS<T> {
    fn heap_size_of_children(&self) -> usize {
        // See comment on HeapSizeOf for JS<T>.
        0
    }
}

/// A holder that allows to lazily initialize the value only once
/// `JS<T>`, using OnceCell
/// Essentially a `OnceCell<JS<T>>`.
///
/// This should only be used as a field in other DOM objects; see warning
/// on `JS<T>`.
#[must_root]
pub struct OnceCellJS<T: DomObject> {
    ptr: OnceCell<JS<T>>,
}

impl<T: DomObject> OnceCellJS<T> {
    /// Retrieve a copy of the current inner value. If it is `None`, it is
    /// initialized with the result of `cb` first.
    #[allow(unrooted_must_root)]
    pub fn init_once<F>(&self, cb: F) -> &T
        where F: FnOnce() -> Root<T>
    {
        debug_assert!(thread_state::get().is_script());
        &self.ptr.init_once(|| JS::from_ref(&cb()))
    }
}

impl<T: DomObject> Default for OnceCellJS<T> {
    #[allow(unrooted_must_root)]
    fn default() -> OnceCellJS<T> {
        debug_assert!(thread_state::get().is_script());
        OnceCellJS {
            ptr: OnceCell::new(),
        }
    }
}

impl<T: DomObject> HeapSizeOf for OnceCellJS<T> {
    fn heap_size_of_children(&self) -> usize {
        // See comment on HeapSizeOf for JS<T>.
        0
    }
}

#[allow(unrooted_must_root)]
unsafe impl<T: DomObject> JSTraceable for OnceCellJS<T> {
    unsafe fn trace(&self, trc: *mut JSTracer) {
        if let Some(ptr) = self.ptr.as_ref() {
            ptr.trace(trc);
        }
    }
}

impl<T: DomObject> LayoutJS<T> {
    /// Returns an unsafe pointer to the interior of this JS object. This is
    /// the only method that be safely accessed from layout. (The fact that
    /// this is unsafe is what necessitates the layout wrappers.)
    pub unsafe fn unsafe_get(&self) -> *const T {
        debug_assert!(thread_state::get().is_layout());
        self.ptr.get()
    }

    /// Returns a reference to the interior of this JS object. This method is
    /// safe to call because it originates from the layout thread, and it cannot
    /// mutate DOM nodes.
    pub fn get_for_script(&self) -> &T {
        debug_assert!(thread_state::get().is_script());
        unsafe { &*self.ptr.get() }
    }
}

/// Get a reference out of a rooted value.
pub trait RootedReference<'root> {
    /// The type of the reference.
    type Ref: 'root;
    /// Obtain a reference out of the rooted value.
    fn r(&'root self) -> Self::Ref;
}

impl<'root, T: JSTraceable + DomObject + 'root> RootedReference<'root> for [JS<T>] {
    type Ref = &'root [&'root T];
    fn r(&'root self) -> &'root [&'root T] {
        unsafe { mem::transmute(self) }
    }
}

impl<'root, T: DomObject + 'root> RootedReference<'root> for Rc<T> {
    type Ref = &'root T;
    fn r(&'root self) -> &'root T {
        self
    }
}

impl<'root, T: RootedReference<'root> + 'root> RootedReference<'root> for Option<T> {
    type Ref = Option<T::Ref>;
    fn r(&'root self) -> Option<T::Ref> {
        self.as_ref().map(RootedReference::r)
    }
}

/// A rooting mechanism for reflectors on the stack.
/// LIFO is not required.
///
/// See also [*Exact Stack Rooting - Storing a GCPointer on the CStack*]
/// (https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/Internals/GC/Exact_Stack_Rooting).
pub struct RootCollection {
    roots: UnsafeCell<Vec<*const Reflector>>,
}

/// A pointer to a RootCollection, for use in global variables.
pub struct RootCollectionPtr(pub *const RootCollection);

impl Copy for RootCollectionPtr {}
impl Clone for RootCollectionPtr {
    fn clone(&self) -> RootCollectionPtr {
        *self
    }
}

impl RootCollection {
    /// Create an empty collection of roots
    pub fn new() -> RootCollection {
        debug_assert!(thread_state::get().is_script());
        RootCollection {
            roots: UnsafeCell::new(vec![]),
        }
    }

    /// Start tracking a stack-based root
    unsafe fn root(&self, untracked_reflector: *const Reflector) {
        debug_assert!(thread_state::get().is_script());
        let roots = &mut *self.roots.get();
        roots.push(untracked_reflector);
        assert!(!(*untracked_reflector).get_jsobject().is_null())
    }

    /// Stop tracking a stack-based reflector, asserting if it isn't found.
    unsafe fn unroot(&self, tracked_reflector: *const Reflector) {
        assert!(!tracked_reflector.is_null());
        assert!(!(*tracked_reflector).get_jsobject().is_null());
        debug_assert!(thread_state::get().is_script());
        let roots = &mut *self.roots.get();
        match roots.iter().rposition(|r| *r == tracked_reflector) {
            Some(idx) => {
                roots.remove(idx);
            },
            None => panic!("Can't remove a root that was never rooted!"),
        }
    }
}

/// SM Callback that traces the rooted reflectors
pub unsafe fn trace_roots(tracer: *mut JSTracer) {
    debug!("tracing stack roots");
    STACK_ROOTS.with(|ref collection| {
        let RootCollectionPtr(collection) = collection.get().unwrap();
        let collection = &*(*collection).roots.get();
        for root in collection {
            trace_reflector(tracer, "on stack", &**root);
        }
    });
}

/// A rooted reference to a DOM object.
///
/// The JS value is pinned for the duration of this object's lifetime; roots
/// are additive, so this object's destruction will not invalidate other roots
/// for the same JS value. `Root`s cannot outlive the associated
/// `RootCollection` object.
#[allow_unrooted_interior]
pub struct Root<T: DomObject> {
    /// Reference to rooted value that must not outlive this container
    ptr: NonZero<*const T>,
    /// List that ensures correct dynamic root ordering
    root_list: *const RootCollection,
}

impl<T: Castable> Root<T> {
    /// Cast a DOM object root upwards to one of the interfaces it derives from.
    pub fn upcast<U>(root: Root<T>) -> Root<U>
        where U: Castable,
              T: DerivedFrom<U>
    {
        unsafe { mem::transmute(root) }
    }

    /// Cast a DOM object root downwards to one of the interfaces it might implement.
    pub fn downcast<U>(root: Root<T>) -> Option<Root<U>>
        where U: DerivedFrom<T>
    {
        if root.is::<U>() {
            Some(unsafe { mem::transmute(root) })
        } else {
            None
        }
    }
}

impl<T: DomObject> Root<T> {
    /// Create a new stack-bounded root for the provided JS-owned value.
    /// It cannot outlive its associated `RootCollection`, and it gives
    /// out references which cannot outlive this new `Root`.
    pub fn new(unrooted: NonZero<*const T>) -> Root<T> {
        debug_assert!(thread_state::get().is_script());
        STACK_ROOTS.with(|ref collection| {
            let RootCollectionPtr(collection) = collection.get().unwrap();
            unsafe { (*collection).root(&*(*unrooted.get()).reflector()) }
            Root {
                ptr: unrooted,
                root_list: collection,
            }
        })
    }

    /// Generate a new root from a reference
    pub fn from_ref(unrooted: &T) -> Root<T> {
        Root::new(unsafe { NonZero::new_unchecked(unrooted) })
    }
}

impl<'root, T: DomObject + 'root> RootedReference<'root> for Root<T> {
    type Ref = &'root T;
    fn r(&'root self) -> &'root T {
        self
    }
}

impl<T: DomObject> Deref for Root<T> {
    type Target = T;
    fn deref(&self) -> &T {
        debug_assert!(thread_state::get().is_script());
        unsafe { &*self.ptr.get() }
    }
}

impl<T: DomObject + HeapSizeOf> HeapSizeOf for Root<T> {
    fn heap_size_of_children(&self) -> usize {
        (**self).heap_size_of_children()
    }
}

impl<T: DomObject> PartialEq for Root<T> {
    fn eq(&self, other: &Self) -> bool {
        self.ptr == other.ptr
    }
}

impl<T: DomObject> Clone for Root<T> {
    fn clone(&self) -> Root<T> {
        Root::from_ref(&*self)
    }
}

impl<T: DomObject> Drop for Root<T> {
    fn drop(&mut self) {
        unsafe {
            (*self.root_list).unroot(self.reflector());
        }
    }
}

unsafe impl<T: DomObject> JSTraceable for Root<T> {
    unsafe fn trace(&self, _: *mut JSTracer) {
        // Already traced.
    }
}

/// Helper trait for safer manipulations of Option<Heap<T>> values.
pub trait OptionalHeapSetter {
    type Value;
    /// Update this optional heap value with a new value.
    fn set(&mut self, v: Option<Self::Value>);
}

impl<T: GCMethods + Copy> OptionalHeapSetter for Option<Heap<T>> where Heap<T>: Default {
    type Value = T;
    fn set(&mut self, v: Option<T>) {
        let v = match v {
            None => {
                *self = None;
                return;
            }
            Some(v) => v,
        };

        if self.is_none() {
            *self = Some(Heap::default());
        }

        self.as_ref().unwrap().set(v);
    }
}
