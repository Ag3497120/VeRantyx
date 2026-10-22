Buffer<uint> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    float a0 = asfloat(inputBuffer[0]);
    float b0 = asfloat(inputBuffer[1]);
    float a1 = asfloat(inputBuffer[2]);
    float b1 = asfloat(inputBuffer[3]);
    float a2 = asfloat(inputBuffer[4]);
    float b2 = asfloat(inputBuffer[5]);
    float a3 = asfloat(inputBuffer[6]);
    float b3 = asfloat(inputBuffer[7]);
    float a4 = asfloat(inputBuffer[8]);
    float b4 = asfloat(inputBuffer[9]);
    float a5 = asfloat(inputBuffer[10]);
    float b5 = asfloat(inputBuffer[11]);

    outputBuffer[0] = asuint(min(a0, b0));
    outputBuffer[1] = asuint(max(a0, b0));
    outputBuffer[2] = asuint(min(a1, b1));
    outputBuffer[3] = asuint(max(a1, b1));
    outputBuffer[4] = asuint(min(a2, b2));
    outputBuffer[5] = asuint(max(a2, b2));
    outputBuffer[6] = asuint(min(a3, b3));
    outputBuffer[7] = asuint(max(a3, b3));
    outputBuffer[8] = asuint(min(a4, b4));
    outputBuffer[9] = asuint(max(a4, b4));
    outputBuffer[10] = asuint(min(a5, b5));
    outputBuffer[11] = asuint(max(a5, b5));
}
