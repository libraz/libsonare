# macOS audio input check

Build with `BUILD_LIVE_DEVICES=ON`. Run the host from an app or terminal with microphone access enabled in System Settings → Privacy & Security → Microphone. Use a microphone with a known working input signal.

1. Attach a caller-owned stereo buffer with `RealtimeEngine::set_capture_segment`. Select `CaptureSource::kInput`, call `set_capture_armed(true)`, and use `set_input_monitor(false, 1.0f)` to avoid feedback.
2. Set `LiveSession::SelectorConfig::audio_input.selection` to `DeviceSelection::kDefault` and leave output and MIDI unselected. Open the session and play or speak into the microphone briefly. Record `status()` while it is open, then close it before reading `captured_frames()` and the buffer. Confirm the capture contains frames and that its level rises with the input signal. For a mono device, compare the two recorded planes.
3. Repeat with input and output on one duplex interface. Repeat again with separate devices, such as the built-in microphone and speakers. Set both devices to a common rate in Audio MIDI Setup. Confirm capture starts in both configurations. On separate devices, check the input overrun and underrun frame counts after the ring has primed.

Record the device names, negotiated rate and block sizes, captured frame count, and ring counters. A headless process without microphone access can receive silent input. Use a permitted process for this check.
