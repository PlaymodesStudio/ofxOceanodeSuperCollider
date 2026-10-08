/*
    OceanodeRadioIn
    Reads stereo audio from the POSIX shared-memory ring buffer written by
    ofxOceanodeOnlineRadio (radioStationVLC node in Oceanode).

    Usage:
        { OceanodeRadioIn.ar(0) }.play   // instance 0
        { OceanodeRadioIn.ar(1) }.play   // instance 1

    Returns an Array of 2 audio-rate signals [left, right].

    Notes:
    - instanceId must match the radio node's instance ID (assigned in order of
      creation: first RadioStationVLC node = 0, second = 1, …).
    - The openFrameworks addon must be running and the radio node must have
      started streaming before you instantiate this UGen.
    - Toggle "Output to SC" on the radio node to mute system audio output
      while routing to SC.
    - If the radio restarts (station change, etc.), the UGen automatically
      resyncs via the generation counter — no need to free and re-create it.
    - Sample rate: the SHM always contains 44100 Hz audio (VLC transcode).
      Start the SC server at 44100 Hz for correct pitch.
          s = Server.default;
          s.options.sampleRate = 44100;
          s.reboot;
*/

OceanodeRadioIn : MultiOutUGen {

    *ar { |instanceId = 0|
        ^this.multiNew(\audio, instanceId)
    }

    init {
        ^this.initOutputs(2, rate)
    }

    checkInputs {
        ^this.checkNInputs(1)
    }
}
