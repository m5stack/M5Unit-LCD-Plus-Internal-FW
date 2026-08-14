# Unit LCD Plus Encoder Protocol

Firmware 0.5 adds rotary encoder and push-button input without changing any
existing Unit LCD command. Multi-byte values below use big-endian byte order.
The complete Chinese protocol specification is in
[`encoder_button_protocol_zh.md`](encoder_button_protocol_zh.md).

## Commands

| Command | Request | Response | Purpose |
| --- | --- | --- | --- |
| `0xB0 READ_ENCODER_STATE` | 1 byte | 8 bytes | Read current state and queue counters |
| `0xB1 PEEK_ENCODER_EVENT` | 1 byte | 10 bytes | Read the head event without removing it |
| `0xB2 ACK_ENCODER_EVENT` | 3 bytes | None | Remove the head event if its sequence matches |
| `0xB3 SET_ENCODER_POSITION` | 5 bytes | None | Set position and clear the queue and overflow count |
| `0xB4 READ_BUTTON_CONFIG` | 1 byte | 2 bytes | Read the long-press threshold in milliseconds |
| `0xB5 SET_BUTTON_CONFIG` | 3 bytes | None | Set the long-press threshold in milliseconds |

`READ_ENCODER_STATE` returns:

```text
Byte 0      flags
Byte 1      pending_count (0..64)
Byte 2..5   position (signed int32, big endian)
Byte 6..7   dropped_count (uint16, big endian)
```

Flag bit 0 means the button is pressed, bit 1 means the current press has
reached the long-press threshold, bit 2 means an event is pending, and bit 7
means at least one event was dropped. Other bits are zero.

`PEEK_ENCODER_EVENT` returns:

```text
Byte 0      event_type
Byte 1      event_flags
Byte 2..3   sequence (uint16, big endian)
Byte 4..5   delta (signed int16, big endian)
Byte 6..9   position (signed int32, big endian)
```

Event types are `0 NONE`, `1 ROTATE`, `2 PRESS`, `3 RELEASE`, `4 SINGLE_CLICK`,
`5 LONG_PRESS`, and `6 DOUBLE_CLICK`. An empty queue returns `NONE`, sequence `0xFFFF`, zero delta,
and the current position and button flags.

The host must ACK the sequence after receiving a complete event. A missing,
duplicate, stale, or out-of-order ACK has no effect. A short click produces
`PRESS`, `RELEASE`, then `SINGLE_CLICK` after the fixed 300 ms double-click
window. A double click produces `PRESS`, `RELEASE`, `PRESS`, `RELEASE`, and
`DOUBLE_CLICK`, without single-click events. A long press produces `PRESS`,
`LONG_PRESS`, `RELEASE`. Rotation reports unaccelerated `+1` or `-1` steps. The signed
position saturates at the int32 limits.

The fixed queue contains 64 events. When full, the oldest event is discarded,
the newest event is retained, and the saturating `dropped_count` is incremented.
`SET_ENCODER_POSITION` uses `command + signed int32` and clears queued events
and overflow state without changing the physical button state or next sequence.

Input timing is 1 ms sampling, 10 ms button debounce, a fixed 300 ms
double-click window, and a configurable 500..5000 ms long-press threshold that
defaults to 500 ms. `READ_BUTTON_CONFIG` returns the threshold as uint16 BE;
`SET_BUTTON_CONFIG` accepts command plus uint16 BE. Invalid values have no
effect, and reset restores 500 ms. Encoder A/B/button use ESP32-C61
GPIO2/GPIO3/GPIO4; the button is
active low with internal pull-ups enabled.
