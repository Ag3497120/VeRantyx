// A DLL loaded only at run time (LoadLibrary/GetProcAddress), with an export by ordinal and a forwarder.
int plugin_value(void) { return 7; }
// A count kept in the DLL's statics: it starts over when the DLL is unloaded and loaded again.
static int calls;
int plugin_calls(void) { return ++calls; }
