Buffer<uint> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    uint unsignedLow = inputBuffer[0];
    uint unsignedHigh = inputBuffer[1];
    int signedLow = asint(inputBuffer[2]);
    int signedHigh = asint(inputBuffer[3]);

    outputBuffer[0] = min(unsignedLow, unsignedHigh);
    outputBuffer[1] = max(unsignedLow, unsignedHigh);
    outputBuffer[2] = asuint(min(signedLow, signedHigh));
    outputBuffer[3] = asuint(max(signedLow, signedHigh));
}
