# 0081 — The browser autoplay policy and the user gesture

Date: 2026-10-25

Status: Accepted

## Context

A browser refuses to let a page make sound until the user has interacted with it.
The mechanism is the AudioContext's initial state: a context is created
`suspended`, and only a real gesture — a click, a key press, a touch — may
`resume()` it. The rule exists to stop a page that the user never asked for from
blaring audio, and it applies to every browser the web target ships to.

miniaudio, the engine's audio backend, does not hide this. Its WebAudio backend
creates the context suspended on purpose, and its `ma_device_start__webaudio()`
sets the JS-side device state to `started` *before* the `resume()` promise has
resolved. So `ma_device_start()` returns success, the engine marks the device
playing, and the mixer submits frames that the browser silently discards. The
engine's `AudioDevice` interface had no way to express "started, but not yet
audible", so a web run could not tell a page waiting for a gesture apart from a
page whose audio was broken. `web-audio-context` (docs/SHIPPING_QUEUE.md) asks for exactly
that distinction: the device must tolerate a suspended context and play after a
resume, rather than crash or lie about its state.

## Decision

Model the autoplay policy as an explicit, testable state on the audio device
seam, not as web-only special-casing inside the mixer.

`AudioDevice` gains three methods, all with defaults that keep every
non-browser backend unchanged:

* `needs_user_gesture()` — false everywhere except a web device whose context
  opened suspended.
* `is_suspended()` — true while the device is started but the platform is still
  withholding sound for want of a gesture. Only meaningful while playing, so a
  caller reads "started" and "suspended" together to distinguish "waiting for the
  user" from "stopped" or "no output".
* `resume_from_gesture()` — tells the device the user has interacted. Idempotent,
  a no-op where no gesture is needed, and it returns whether the device can now
  play.

`MiniAudioDevice` implements the three. On the web it sets the awaiting-gesture
flag when it opens a context (the context is created suspended), resumes the
context from JavaScript in `resume_from_gesture()`, and clears the flag when
miniaudio's `unlocked` device notification arrives — the authoritative signal
that the browser actually resumed. On desktop the flag is never set and all three
methods are inert. `NullAudioDevice` gains `set_starts_suspended()`, so a
headless test can model a suspended context exactly: a started device that
accepts nothing until a gesture resumes it.

The gesture itself is the engine's, not the page's. `InputState::any_pressed()`
reports whether any key, mouse button, or touch contact began this frame — a user
gesture in the platform's sense — and `AudioSystem::update()` resumes a suspended
device on the first such gesture of a frame. That keeps the policy in the engine
rather than duplicated in every game, and it means the frame the gesture arrives
in is also the frame its audio becomes audible.

## Consequences

* A web run no longer silently lies: a game can read `is_suspended()` to know the
  page is waiting for a gesture, and `resume_from_gesture()` is the single call
  that resolves it. Nothing crashes before the first gesture, which was the item's
  minimum bar.
* The behaviour is testable with no browser: a `NullAudioDevice` configured
  suspended reproduces the whole path, so `web-audio-context`'s acceptance check
  runs in the ordinary unit suite rather than needing headless Chromium.
* Desktop is untouched by construction. The flag is never set off the web, and
  the default implementations make the three methods no-ops, so no existing
  backend or test changes behaviour.
* The resume is deliberately per-device and not per-source: once the context is
  resumed every voice is audible, which matches the browser's rule (one gesture
  unlocks the page, not one sound).
* The engine still does not draw a "click to start" prompt. `is_suspended()`
  gives a game what it needs to draw one, and a shipped game that wants a silent
  start does not have to.

## Alternatives considered

* **Resume from a DOM event listener in the window backend.** Rejected: it would
  put an audio concern in the platform window layer, and it could not be tested
  without a browser. The gesture is already visible to the engine as input; the
  device should be told about it through the audio seam.
* **Assume the context resumes and keep playing.** Rejected: this is the bug.
  miniaudio's `start` reports success while the browser discards the output, so
  "assume it works" is indistinguishable from the broken state the item is about.
* **Rely on miniaudio's own `unlock` listener.** miniaudio already installs a
  `click`/`touchend` listener that resumes every started device, which is why a
  plain page often does start on the first click. Rejected as the *only* mechanism
  because it fires outside the engine's frame — the engine would learn of the
  resume asynchronously through the `unlocked` notification and could not act on
  the same frame — and because it does not cover a keyboard-only interaction.
  The notification is still honoured as the authoritative confirmation.
* **A separate `WebAudioDevice` backend.** Rejected: the difference is one piece
  of state and one JavaScript call, not a different device. Keeping one backend
  with a platform-conditional branch is smaller and leaves the non-web path with
  no code to review.
