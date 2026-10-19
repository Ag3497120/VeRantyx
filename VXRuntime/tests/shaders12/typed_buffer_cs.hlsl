Buffer<uint> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    uint count;
    inputBuffer.GetDimensions(count);
    outputBuffer[1] = inputBuffer[1] + count;
}
