Buffer<uint> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    int signedValue = asint(inputBuffer[0]);
    float signedFloat = (float)signedValue;
    int truncated = (int)(signedFloat * 1.5f + 0.25f);
    outputBuffer[0] = asuint(signedFloat);
    outputBuffer[1] = asuint(truncated);
}
