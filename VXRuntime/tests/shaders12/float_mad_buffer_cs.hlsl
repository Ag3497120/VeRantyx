Buffer<uint> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    float a = asfloat(inputBuffer[0]);
    float b = asfloat(inputBuffer[1]);
    float c = asfloat(inputBuffer[2]);
    precise float result = mad(a, b, c);
    outputBuffer[0] = asuint(result);
}
