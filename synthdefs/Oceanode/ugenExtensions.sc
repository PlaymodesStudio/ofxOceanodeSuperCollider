// Extended ugenExtensions.sc to handle FFT-related operations

+ UGen {
    at { |index|
        ^this.asArray[index]
    }

    fftSize {
        ^this.inputs[0].fftSize
    }

    // Add support for comparison operator
    > { |other|
        ^BinaryOpUGen('>', this, other)
    }
}

+ UnaryOpUGen {
    at { |index|
        ^this.asArray[index]
    }

    fftSize {
        ^this.a.fftSize
    }
}

+ BinaryOpUGen {
    at { |index|
        ^this.asArray[index]
    }

    fftSize {
        ^this.a.fftSize
    }
}

+ LocalBuf {
    fftSize {
        ^this.numFrames
    }

    numFrames {
        ^this.inputs[0]
    }
}

+ FFT {
    fftSize {
        ^this.inputs[0].fftSize
    }
}

+ BufFFTTrigger {
    fftSize {
        ^this.inputs[0].numFrames
    }
}

+ BufFFT_BufCopy {
    fftSize {
        ^this.inputs[0].fftSize
    }
}

+ BufFFT {
    fftSize {
        ^this.inputs[0].fftSize
    }
}

+ PV_Diffuser {
    fftSize {
        ^this.inputs[0].fftSize
    }
}

+ BufIFFT {
    fftSize {
        ^this.inputs[0].fftSize
    }
}