//! `Base::pick_folder_bookmark`: the system's folder picker,
//! `UIDocumentPickerViewController`, opened on `public.folder` without a copy.
//!
//! A picked folder's URL is what a security-scoped bookmark is made from
//! (`bookmarks.rs`); the picker is the only way iOS lets the app reach a
//! folder outside its own container. The view controller is presented over
//! the key window's top controller, because this module has no view of its
//! own.

use std::cell::RefCell;
use std::sync::mpsc;
use std::sync::Mutex;

use dispatch2::DispatchQueue;
use objc2::rc::Retained;
use objc2::runtime::AnyObject;
use objc2::{
    define_class, msg_send, DefinedClass, MainThreadMarker, MainThreadOnly,
};
use objc2_foundation::{NSArray, NSObject, NSObjectProtocol, NSString, NSURL};
use objc2_ui_kit::{UIApplication, UIDocumentPickerViewController, UIViewController};
#[allow(deprecated)]
use objc2_ui_kit::UIDocumentPickerMode;

/// What one picker answers: the folder picked, `None` when the user
/// cancelled (a button, a swipe), or why it could not be shown.
type Answer = mpsc::Sender<Result<Option<Retained<NSURL>>, String>>;

// The pickers on show (a picker holds its delegate weakly).
thread_local! {
    static SHOWN: RefCell<Vec<Retained<FolderPickerDelegate>>> = const { RefCell::new(Vec::new()) };
}

define_class!(
    /// The delegate of one folder picker.
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[ivars = Mutex<Option<Answer>>]
    struct FolderPickerDelegate;

    unsafe impl NSObjectProtocol for FolderPickerDelegate {}

    impl FolderPickerDelegate {
        #[unsafe(method(documentPicker:didPickDocumentsAtURLs:))]
        fn did_pick(&self, _picker: &UIDocumentPickerViewController, urls: &NSArray<NSURL>) {
            self.answer(urls.iter().next());
        }

        #[unsafe(method(documentPickerWasCancelled:))]
        fn was_cancelled(&self, _picker: &UIDocumentPickerViewController) {
            self.answer(None);
        }

        /// A sheet swiped down (iOS 13+'s card presentation): no folder. The
        /// picker's own callbacks may not come then; whichever comes first
        /// answers.
        #[unsafe(method(presentationControllerDidDismiss:))]
        fn did_dismiss(&self, _controller: &AnyObject) {
            self.answer(None);
        }
    }
);

impl FolderPickerDelegate {
    /// Answers once: a second callback (a swipe after the cancel button, or
    /// the reverse) finds nothing to send to.
    fn answer(&self, url: Option<Retained<NSURL>>) {
        let sender = self
            .ivars()
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .take();
        if let Some(sender) = sender {
            let _ = sender.send(Ok(url));
        }
        let this: *const Self = self;
        SHOWN.with_borrow_mut(|shown| shown.retain(|shown| !std::ptr::eq(&**shown, this)));
    }
}

/// Shows the system's folder picker and waits for the folder the user chose.
/// `Ok(None)` when the user cancelled; `Err` when the picker cannot be shown
/// (no window yet, no root controller).
pub fn pick_folder() -> Result<Option<Retained<NSURL>>, String> {
    let (sender, receiver) = mpsc::channel();
    // UIKit presents on the main thread only; a task of the engine may run
    // elsewhere. `exec_sync` runs inline when it is already the main thread.
    if MainThreadMarker::new().is_some() {
        present(sender);
    } else {
        DispatchQueue::main().exec_sync(move || present(sender));
    }
    receiver
        .recv()
        .map_err(|_| "E_PICKER: 文件夹选择框没有回复".to_owned())?
}

fn present(sender: Answer) {
    let Some(mtm) = MainThreadMarker::new() else {
        let _ = sender.send(Err("E_PICKER: 不在主线程".to_owned()));
        return;
    };
    let Some(top) = top_controller(mtm) else {
        let _ = sender.send(Err("E_PICKER: 找不到可以呈现的窗口".to_owned()));
        return;
    };
    let folder = NSString::from_str("public.folder");
    let types = NSArray::from_slice(&[&*folder]);
    #[allow(deprecated)]
    // SAFETY: UIKit's documented initializer, on the main thread.
    let picker = UIDocumentPickerViewController::initWithDocumentTypes_inMode(
        UIDocumentPickerViewController::alloc(mtm),
        &types,
        UIDocumentPickerMode::Open,
    );
    picker.setAllowsMultipleSelection(false);
    let delegate = FolderPickerDelegate::alloc(mtm).set_ivars(Mutex::new(Some(sender)));
    // SAFETY: the documented init of an NSObject subclass made here.
    let delegate: Retained<FolderPickerDelegate> = unsafe { msg_send![super(delegate), init] };
    // SAFETY: a UIDocumentPickerViewController's delegate is this protocol
    // object; the picker holds it weakly, and SHOWN below keeps it alive.
    unsafe {
        let _: () = msg_send![&picker, setDelegate: &*delegate];
        if let Some(sheet) = picker.presentationController() {
            let _: () = msg_send![&sheet, setDelegate: &*delegate];
        }
    }
    SHOWN.with_borrow_mut(|shown| shown.push(delegate));
    top.presentViewController_animated_completion(&picker, true, None);
}

/// The controller a sheet can be presented from: the key window's root, and
/// whatever it already presents, up to the last one still on screen.
fn top_controller(mtm: MainThreadMarker) -> Option<Retained<UIViewController>> {
    let application = UIApplication::sharedApplication(mtm);
    #[allow(deprecated)]
    let windows = application.windows();
    let window = windows
        .iter()
        .find(|window| window.isKeyWindow())
        .or_else(|| windows.iter().next())?;
    let mut top = window.rootViewController()?;
    while let Some(next) = top.presentedViewController() {
        if next.isBeingDismissed() {
            break;
        }
        top = next;
    }
    Some(top)
}
