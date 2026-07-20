// =============================================================================
//  WaveGrain II — DropOverlay.mm  (macOS only; see DropOverlay.h)
// =============================================================================
#include "DropOverlay.h"

#if JUCE_MAC

#import <AppKit/AppKit.h>

// -----------------------------------------------------------------------------
//  The transparent NSView that receives drags. Owns nothing; talks back to the
//  C++ side through blocks that capture copies of the callbacks.
// -----------------------------------------------------------------------------
@interface WgDropOverlayView : NSView <NSDraggingDestination>
{
@public
    std::function<void(const juce::String&)> onFile;      // marshalled to MM
    std::function<void(bool)>                onHighlight; // marshalled to MM
    NSOperationQueue*                        workQueue;   // promise resolution (NOT main)
    NSString*                                tempDir;     // per-instance promise dest
}
@end

@implementation WgDropOverlayView

- (NSView*)hitTest:(NSPoint)point
{
    juce::ignoreUnused(point);
    return nil;   // drags only — every ordinary mouse event passes through
}

- (BOOL)wantsPeriodicDraggingUpdates { return NO; }

- (void)notifyHighlight:(BOOL)over
{
    auto cb = onHighlight;
    if (!cb) return;
    const bool o = over;
    juce::MessageManager::callAsync([cb, o] { cb(o); });
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    juce::ignoreUnused(sender);
    [self notifyHighlight:YES];
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    juce::ignoreUnused(sender);
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    juce::ignoreUnused(sender);
    [self notifyHighlight:NO];
}

- (void)draggingEnded:(id<NSDraggingInfo>)sender
{
    juce::ignoreUnused(sender);
    [self notifyHighlight:NO];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    juce::ignoreUnused(sender);
    return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    __block bool handledAnything = false;
    auto fileCb = onFile;

    NSURL* destDir = [NSURL fileURLWithPath:tempDir isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:destDir
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:nil];

    NSArray* classes = @[ [NSFilePromiseReceiver class], [NSURL class] ];
    NSDictionary* searchOptions = @{ NSPasteboardURLReadingFileURLsOnlyKey : @YES };

    [sender enumerateDraggingItemsWithOptions:0
                                      forView:self
                                      classes:classes
                                searchOptions:searchOptions
                                   usingBlock:^(NSDraggingItem* draggingItem, NSInteger idx, BOOL* stop)
    {
        juce::ignoreUnused(idx, stop);
        id item = draggingItem.item;

        if ([item isKindOfClass:[NSFilePromiseReceiver class]])
        {
            NSFilePromiseReceiver* receiver = (NSFilePromiseReceiver*) item;
            [receiver receivePromisedFilesAtDestination:destDir
                                                options:@{}
                                         operationQueue:workQueue
                                                 reader:^(NSURL* fileURL, NSError* error)
            {
                if (error != nil || fileURL == nil || !fileCb) return;
                juce::String path = juce::String::fromUTF8([[fileURL path] UTF8String]);
                // Promise files are our own temp copies: the callback loads the
                // sample into memory synchronously, so delete the copy after it
                // returns (real user files take the NSURL branch and are kept).
                juce::MessageManager::callAsync([fileCb, path] { fileCb(path); juce::File(path).deleteFile(); });
            }];
            handledAnything = true;
        }
        else if ([item isKindOfClass:[NSURL class]])
        {
            NSURL* url = (NSURL*) item;
            if ([url isFileURL] && fileCb)
            {
                juce::String path = juce::String::fromUTF8([[url path] UTF8String]);
                juce::MessageManager::callAsync([fileCb, path] { fileCb(path); });
                handledAnything = true;
            }
        }
    }];

    [self notifyHighlight:NO];
    return handledAnything ? YES : NO;
}

@end

// -----------------------------------------------------------------------------
//  C++ bridge
// -----------------------------------------------------------------------------
namespace wgui
{
struct DropOverlayImpl
{
    WgDropOverlayView* view = nil;
    NSOperationQueue*  queue = nil;
    NSString*          tempDir = nil;
};

DropOverlay::DropOverlay(juce::ComponentPeer& peer, Callbacks cbs)
{
    NSView* peerView = (NSView*) peer.getNativeHandle();
    if (peerView == nil) return;

    auto* impl = new DropOverlayImpl();

    impl->queue = [[NSOperationQueue alloc] init];
    impl->queue.maxConcurrentOperationCount = 1;
    impl->queue.qualityOfService = NSQualityOfServiceUserInitiated;

    NSString* base = NSTemporaryDirectory();
    impl->tempDir = [[base stringByAppendingPathComponent:
        [NSString stringWithFormat:@"wg2-drop-%@", [[NSUUID UUID] UUIDString]]] retain];

    WgDropOverlayView* v = [[WgDropOverlayView alloc] initWithFrame:[peerView bounds]];
    v->onFile      = std::move(cbs.onFile);
    v->onHighlight = std::move(cbs.onHighlight);
    v->workQueue   = impl->queue;
    v->tempDir     = impl->tempDir;
    [v setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    NSMutableArray* types = [[NSFilePromiseReceiver readableDraggedTypes] mutableCopy];
    [types addObject:NSPasteboardTypeFileURL];
    [v registerForDraggedTypes:types];
    [types release];

    [peerView addSubview:v];   // topmost subview -> wins the destination search
    impl->view = v;
    impl_ = impl;
}

DropOverlay::~DropOverlay()
{
    auto* impl = static_cast<DropOverlayImpl*>(impl_);
    if (impl == nullptr) return;

    if (impl->view != nil)
    {
        [impl->view unregisterDraggedTypes];
        [impl->view removeFromSuperview];
        [impl->view release];
    }
    if (impl->queue != nil)
    {
        [impl->queue cancelAllOperations];
        [impl->queue release];
    }
    if (impl->tempDir != nil)
    {
        [[NSFileManager defaultManager] removeItemAtPath:impl->tempDir error:nil];
        [impl->tempDir release];
    }
    delete impl;
    impl_ = nullptr;
}
} // namespace wgui

#endif // JUCE_MAC
