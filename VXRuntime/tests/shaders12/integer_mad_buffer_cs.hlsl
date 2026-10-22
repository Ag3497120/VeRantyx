Buffer<int> inputBuffer : register(t0);
RWBuffer<uint> outputBuffer : register(u0);

[numthreads(1, 1, 1)]
void main() {
    int a = inputBuffer[0];
    int b = inputBuffer[1];
    int c = inputBuffer[2];
    uint ua = asuint(a);
    uint ub = asuint(b);
    uint uc = asuint(c);
    outputBuffer[0] = asuint(mad(a, b, c));
    outputBuffer[1] = mad(ua, ub, uc);
}
