// visionOS (VISIONOS_PORT.md): the system's proxy settings come from CFNetworkCopySystemProxySettings, without
// SCDynamicStore, and the SDK leaves out the keys for HTTPS, SOCKS and auto-configuration. Their values are the same
// strings in both families of names. Included after the SDK's headers (src/winhttp.m, src/winhttp_proxy.m).
#pragma once
#include <CFNetwork/CFNetwork.h>
#define SCDynamicStoreCopyProxies(store) CFNetworkCopySystemProxySettings()
#undef kSCPropNetProxiesHTTPEnable
#define kSCPropNetProxiesHTTPEnable CFSTR("HTTPEnable")
#undef kSCPropNetProxiesHTTPProxy
#define kSCPropNetProxiesHTTPProxy CFSTR("HTTPProxy")
#undef kSCPropNetProxiesHTTPPort
#define kSCPropNetProxiesHTTPPort CFSTR("HTTPPort")
#undef kSCPropNetProxiesHTTPSEnable
#define kSCPropNetProxiesHTTPSEnable CFSTR("HTTPSEnable")
#undef kSCPropNetProxiesHTTPSProxy
#define kSCPropNetProxiesHTTPSProxy CFSTR("HTTPSProxy")
#undef kSCPropNetProxiesHTTPSPort
#define kSCPropNetProxiesHTTPSPort CFSTR("HTTPSPort")
#undef kSCPropNetProxiesSOCKSEnable
#define kSCPropNetProxiesSOCKSEnable CFSTR("SOCKSEnable")
#undef kSCPropNetProxiesSOCKSProxy
#define kSCPropNetProxiesSOCKSProxy CFSTR("SOCKSProxy")
#undef kSCPropNetProxiesSOCKSPort
#define kSCPropNetProxiesSOCKSPort CFSTR("SOCKSPort")
#undef kSCPropNetProxiesProxyAutoConfigEnable
#define kSCPropNetProxiesProxyAutoConfigEnable CFSTR("ProxyAutoConfigEnable")
#undef kSCPropNetProxiesProxyAutoConfigURLString
#define kSCPropNetProxiesProxyAutoConfigURLString CFSTR("ProxyAutoConfigURLString")
#undef kSCPropNetProxiesProxyAutoDiscoveryEnable
#define kSCPropNetProxiesProxyAutoDiscoveryEnable CFSTR("ProxyAutoDiscoveryEnable")
#undef kSCPropNetProxiesExceptionsList
#define kSCPropNetProxiesExceptionsList CFSTR("ExceptionsList")
#undef kCFNetworkProxiesHTTPSEnable
#define kCFNetworkProxiesHTTPSEnable CFSTR("HTTPSEnable")
#undef kCFNetworkProxiesHTTPSProxy
#define kCFNetworkProxiesHTTPSProxy CFSTR("HTTPSProxy")
#undef kCFNetworkProxiesHTTPSPort
#define kCFNetworkProxiesHTTPSPort CFSTR("HTTPSPort")
#undef kCFNetworkProxiesSOCKSEnable
#define kCFNetworkProxiesSOCKSEnable CFSTR("SOCKSEnable")
#undef kCFNetworkProxiesProxyAutoDiscoveryEnable
#define kCFNetworkProxiesProxyAutoDiscoveryEnable CFSTR("ProxyAutoDiscoveryEnable")
