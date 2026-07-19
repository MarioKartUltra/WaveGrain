// =============================================================================
//  WaveGrain II — DropOverlay.h
//  Native macOS NSDraggingDestination overlay for the plugin editor. JUCE's
//  peer only resolves a legacy promise flavour, so promise-only drags from
//  Ableton's browser (clips Live renders first, pack/cloud items) never reach
//  FileDragAndDropTarget. This transparent NSView subview of the peer accepts
//  NSFilePromiseReceiver + NSPasteboardTypeFileURL, resolves promises into a
//  per-instance temp dir on a background NSOperationQueue, and marshals the
//  resulting path back with MessageManager::callAsync. hitTest returns nil so
//  ordinary mouse events pass through untouched. (TECH_BRIEF section 1.)
//  The regular JUCE FileDragAndDropTarget path stays in place for plain file
//  drags (Finder + most Live browser samples).
// =============================================================================
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC

namespace wgui
{
class DropOverlay
{
public:
    struct Callbacks
    {
        std::function<void(const juce::String& path)> onFile;      // message thread
        std::function<void(bool over)>                onHighlight; // message thread
    };

    // Attach a native overlay covering the peer's NSView. Detach + temp-dir
    // cleanup happens in the destructor. Construct/destroy on the message thread.
    DropOverlay(juce::ComponentPeer& peer, Callbacks cbs);
    ~DropOverlay();

    DropOverlay(const DropOverlay&) = delete;
    DropOverlay& operator=(const DropOverlay&) = delete;

private:
    void* impl_ = nullptr;   // WgDropOverlayImpl*
};
} // namespace wgui

#endif // JUCE_MAC
