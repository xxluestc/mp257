#include "runtime/frame_pipeline.hpp"
#include "runtime/fusion_state.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace helmet;

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    const uint8_t original[] = {1, 2, 3, 4};
    FramePool pool(2, 4);
    auto first = pool.copy(original, 4, 100, 1, 1, 1);
    auto second = pool.copy(original, 4, 200, 2, 1, 1);
    auto borrowed = first;
    first.reset();
    require(!pool.copy(original, 4, 300, 3, 1, 1), "published slot was reused");
    borrowed.reset();
    require(pool.available() == 1, "slot did not return after last reference");
    require(!pool.copy(original, 5, 300, 3, 1, 1), "oversized JPEG accepted");
    auto recycled = pool.copy(original, 4, 300, 3, 1, 1);
    require(second->timestamp_us == 200 && second->bytes[0] == 1, "live frame was overwritten");

    FrameRef survivor;
    {
        FramePool temporary(1, 4);
        survivor = temporary.copy(original, 4, 1, 1, 1, 1);
    }
    require(survivor->bytes[3] == 4, "frame outlived its storage");
    survivor.reset();

    FrameRing ring(1, 150);
    ring.append(second);
    auto snapshot = ring.snapshot(250);
    ring.append(recycled);
    require(snapshot.size() == 1 && snapshot[0]->timestamp_us == 200,
            "ring eviction invalidated the event snapshot");
    ring.expire(500);
    require(ring.snapshot(500).empty(), "stalled camera prevented time eviction");
    require(snapshot[0]->bytes[0] == 1, "snapshot lost immutable contents");

    BoundedQueue<int> queue(2);
    queue.push(1);
    queue.push(2);
    require(!queue.push(3), "reject-newest overflow failed");
    require(queue.push(4, OverflowPolicy::DropOldest), "fresh result rejected");
    int value = 0;
    require(queue.pop(value, std::chrono::milliseconds(0)) && value == 2,
            "drop-oldest ordering failed");
    require(queue.dropped() == 2, "overflow was not counted");
    queue.close();
    require(!queue.push(5), "closed queue accepted work");
    require(queue.pop(value, std::chrono::milliseconds(0)) && value == 4,
            "close discarded queued work");
    require(queue.drained(), "closed queue did not drain");
    BoundedQueue<int> waiting(1);
    auto waiter = std::async(std::launch::async, [&] {
        int number;
        return waiting.pop(number, std::chrono::seconds(5));
    });
    waiting.close();
    require(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !waiter.get(),
            "close did not wake consumer");

    FramePool cancelled_pool(2, 4);
    BoundedQueue<FrameRef> cancelled(2);
    cancelled.push(cancelled_pool.copy(original, 4, 1, 1, 1, 1));
    cancelled.push(cancelled_pool.copy(original, 4, 2, 2, 1, 1));
    cancelled.cancel();
    require(cancelled.drained() && cancelled_pool.available() == 2,
            "cancellation retained queued JPEG ownership");
    cancelled.cancel();
    require(!cancelled.push({}), "cancelled queue accepted work");

    EventWindow event(15, 60);
    event.trigger(100);
    require(!event.due(114) && event.due(115), "post window is incorrect");
    event.trigger(110);
    require(!event.due(120) && event.due(125), "repeat trigger did not extend");
    event.trigger(105);
    require(!event.due(120) && event.due(125), "delayed trigger shortened the event");
    require(event.contains(90, 15) && !event.contains(84, 15) && !event.contains(126, 15),
            "timestamp selection escaped the actual event window");
    event.trigger(159);
    require(event.due(160), "repeat trigger escaped duration cap");
    event.finish();
    event.trigger(200);
    require(event.due(215), "new event inherited previous deadline");
    event.finish();
    event.trigger(UINT64_MAX - 10);
    require(event.end() == UINT64_MAX && !event.due(UINT64_MAX - 1),
            "event deadline overflow wrapped into the past");

    FrameRing delayed_ring(2, 150);
    delayed_ring.append(second);   // 200, before event at 250.
    delayed_ring.append(recycled); // 300, after event but before processing.
    auto delayed = delayed_ring.snapshot_between(100, 310);
    require(delayed.size() == 2 && delayed.back()->timestamp_us == 300,
            "delayed event lost already buffered post-event frames");

    VisionGate vision;
    require(vision.alert(true, false, 1), "radar-only fallback lost alert");
    vision.update(100, true);
    require(!vision.alert(true, true, 100), "single positive confirmed target");
    vision.update(200, true);
    require(vision.alert(true, true, 200), "consecutive positives failed");
    vision.update(300, false);
    vision.update(400, false);
    require(vision.confirmed(), "confirmation cleared before deny threshold");
    vision.update(500, false);
    require(!vision.confirmed(), "three negatives did not clear confirmation");
    require(vision.alert(true, true, 3000000), "stale negative suppressed radar indefinitely");
    vision.update(3000001, true);
    require(!vision.confirmed(), "confirmation survived stale gap");
    vision.update(200, true);
    require(!vision.confirmed(), "out-of-order result changed state");

    // Exercise publication/recycling across real producer and consumer threads.
    FramePool stress_pool(8, 4);
    BoundedQueue<FrameRef> channel(4);
    std::atomic<unsigned> consumed{0};
    std::thread consumer([&] {
        FrameRef frame;
        while (!channel.drained()) {
            if (channel.pop(frame, std::chrono::milliseconds(10))) {
                require(frame->size == 4 && frame->bytes[3] == 4, "torn published JPEG");
                ++consumed;
                frame.reset();
            }
        }
    });
    for (unsigned i = 1; i <= 10000; ++i) {
        auto frame = stress_pool.copy(original, 4, i, i, 1, 1);
        if (frame)
            channel.push(frame, OverflowPolicy::DropOldest);
    }
    channel.close();
    consumer.join();
    require(consumed > 0 && stress_pool.available() == 8, "stress test leaked frame slots");
    std::cout << "Frame ownership, queues, ring, event windows and fusion checks passed\n";
}
