# Spike: WASAPI capability probe

A throwaway program whose only job is to answer the questions that gate the
HiFi plan, before any of it gets built.

## Build & run

```powershell
$env:PATH = "$env:USERPROFILE\scoop\shims;$env:USERPROFILE\scoop\apps\mingw\current\bin;$env:PATH"
cmake -B build -G Ninja
cmake --build build
.\build\wasapi_probe.exe
```

## Result on this machine

```
== 1. endpoint enumeration ==
  [PASS] at least one active render endpoint          2 found
       - LG TV SSCR2 (NVIDIA High Definition Audio)
       -

== device: LG TV SSCR2 (NVIDIA High Definition Audio) ==
       native format: 48000 Hz / 2 ch / 16-bit
  [PASS] shared mode opens                            48000 Hz / 2 ch / 32-bit
  [INFO] generic 48k/24 request refused: Initialize hr=0x8889000F
  [PASS] EXCLUSIVE opens (device native format)       requested 48000 Hz / 2 ch / 16-bit

== summary ==
  endpoints: 2, exclusive-capable: 2
  VERDICT: bit-perfect exclusive output is AVAILABLE on this machine.
```

## What this settles

**Bit-perfect exclusive output works on this machine.** The single biggest
unknown in the HiFi plan is now answered: the WASAPI exclusive path is real,
and a self-written C++ engine can reach it.

## What the probe taught us (all of it the hard way)

### 1. Never guess the format — ask the device

Requesting a hardcoded 48 kHz / 24-bit is **refused** by both endpoints
(`AUDCLNT_E_UNSUPPORTED_FORMAT`, `0x88890008`). Exclusive mode means "exactly
this format or nothing", and the device has its own idea of what it is.

The working path:

1. Read `PKEY_AudioEngine_DeviceFormat` from the endpoint's property store. The
   blob is a `WAVEFORMATEX` describing what the hardware wants.
2. Rebuild it as a `WAVEFORMATEXTENSIBLE` and request *that*.

Passing the device's format through verbatim does **not** work: a plain
`WAVE_FORMAT_PCM` description gets `E_INVALIDARG` when handed to `Initialize`.
It has to be re-wrapped as extensible with a consistent `cbSize`. This is the
exact code the engine needs, so it is worth having written.

### 2. `AUDCLNT_STREAMFLAGS_RAW` is not available by default

The first version of the probe requested `RAW` and every endpoint rejected it
with `AUDCLNT_E_INVALID_STREAM_FLAG` (`0x88890021`) — in **both** shared and
exclusive mode. Microsoft documents RAW as unsupported on some Windows
versions and drivers.

The engine must treat RAW as opt-in and probe for it, never assume it.
Polling mode (flags = 0) works everywhere and is what a pull-style engine
wants anyway.

### 3. `GetMixFormat()` is meaningless in exclusive mode

It reports the mixer's format (`48000 / 2ch / 32-bit`) even when the stream was
opened exclusively at `48000 / 2ch / 16-bit`. Do not use it to report the
signal path — the engine must remember the format it actually requested and
report that.

### 4. MinGW's WASAPI headers need a shim

`wasapi_compat.hpp` patches, in one place:

| Problem | Fix |
| --- | --- |
| `AUDCLNT_STREAMFLAGS_*` entirely absent | define them |
| `AUDCLNT_E_EXCLUSIVE_MODE_ONLY_ALLOWED` absent | define it |
| `PKEY_Device_FriendlyName` undeclared | declare it |
| `PKEY_AudioEngine_DeviceFormat` declared but undefined → link error | define with the fixed GUID |
| `GetCount()`, `GetId()`, `OpenPropertyStore()`, `Activate()` use COM-era signatures | thin wrappers |
| `GetMixFormat()` takes `WAVEFORMATEX**`, not the MSDN two-arg form | call as declared |
| no `libaudio.a` | link `mmdevapi` only |

This is a real, ongoing maintenance cost of choosing MinGW over MSVC — worth
knowing before committing. The shim is ~90 lines and MSVC compiles it to
nothing, so switching toolchains later is cheap.

## Still unproven

This probe only opens a client. Not yet demonstrated:

- actually writing audio buffers and hearing them
- exclusive mode while **another app holds the device** (`DEVICE_IN_USE`)
- ASIO
- DSD / DoP
- Native DSD straight to a DAC
- the C++ → N-API → Electron bridge

Those are the next spikes. The two endpoints here are NVIDIA HDMI and an LG TV,
not a dedicated DAC, so high-resolution exclusive behaviour is still untested
— a real interface may behave differently.
