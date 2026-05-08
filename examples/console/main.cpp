// Beacon C++ SDK - Console Example
//
// Demonstrates the basic lifecycle: configure, identify, track, session, exception, flush.

#include <beacon/beacon.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    // Step 1: Configure the singleton. If required fields are missing,
    // the SDK disables itself silently -- it does NOT throw.
    // The only throw from configure() is std::logic_error if called twice.
    beacon::Tracker::configure([](beacon::Options& opts) {
        opts.api_key      = "sk-live-your-api-key-here";
        opts.api_base_url = "https://your-beacon-instance.com";
        opts.app_name     = "InventoryManager";
        opts.app_version  = "2.1.0";

        // Optional tuning (all have safe defaults):
        // opts.flush_interval_seconds = 60;   // default: 60s
        // opts.max_batch_size         = 25;   // default: 25 events per batch
        // opts.max_queue_size_mb      = 10;   // default: 10 MB offline queue
        // opts.max_breadcrumbs        = 25;   // default: 25 breadcrumbs

        // Register all events this application tracks.
        // The manifest can be exported for the portal's Allowlists Import page.
        opts.events
            .define("app",       "launched")
            .define("inventory", "item_added")
            .define("inventory", "item_deleted")
            .define("inventory", "search_performed")
            .define("reporting", "csv_exported");
    });

    auto tracker = beacon::Tracker::instance();
    if (!tracker) {
        std::cerr << "Beacon configuration failed.\n";
        return 1;
    }

    // Step 2: Identify the user when they log in.
    tracker->identify("user-12345");

    // Step 3: Start a session.
    tracker->startSession(); // Uses the identified actor

    // Step 4: Track events. Returns immediately (fire-and-forget).
    tracker->track("app", "launched");
    tracker->track("inventory", "item_added",
                   std::unordered_map<std::string, std::string>{
                       {"item_type", "widget"}, {"quantity", "5"}
                   });

    // Step 5: Exception tracking (best-effort, fire-and-forget).
    try {
        // ... application logic ...
        throw std::runtime_error("database connection lost");
    } catch (const std::exception& ex) {
        tracker->trackException(ex, beacon::ExceptionSeverity::NonFatal);
    }

    // Step 6: Export event manifest for portal upload (optional).
    tracker->exportEventManifest("beacon_manifest.json");

    // Step 7: End session and flush before exit.
    tracker->endSession();
    tracker->flush(); // Blocks until all queued events are delivered or 30s timeout.

    std::cout << "Beacon example complete.\n";

    // The shared_ptr going out of scope destroys the Tracker:
    // remaining events are persisted to disk, background thread is joined.
    return 0;
}
