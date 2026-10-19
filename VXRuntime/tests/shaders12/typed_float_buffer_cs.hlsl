Buffer<float> inputBuffer : register(t0);
RWBuffer<float> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    uint count;
    inputBuffer.GetDimensions(count);
    outputBuffer[0] = inputBuffer[0] * 1.5f + float(count) * 0.25f;
}
