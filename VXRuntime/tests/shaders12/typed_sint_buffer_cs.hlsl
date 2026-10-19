Buffer<int> inputBuffer : register(t0);
RWBuffer<int> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    uint count;
    inputBuffer.GetDimensions(count);
    int value = inputBuffer[0];
    outputBuffer[1] = value / int(count);
}
