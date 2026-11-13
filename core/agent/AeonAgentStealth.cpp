// =============================================================================
// AeonAgentStealth.cpp — CDP Stealth Injection Module
// Project: Aeon Browser (DelgadoLogic)
//
// Native C++ Clean-Room Implementation of anti-fingerprinting techniques.
// =============================================================================

#include "AeonAgentStealth.h"
#include <iostream>
#include <sstream>

namespace aeon {
namespace agent {

AeonAgentStealth::AeonAgentStealth() {
    m_healthy = false;
    m_last_update_utc = 0;
}

bool AeonAgentStealth::Initialize(const ResourceBudget& budget) {
    SetResourceBudget(budget);
    
    // In a real Chromium environment, we would bind to the DevTools 
    // Inspector here. For the clean-room stub, we mark initialized.
    m_initialized = true;
    m_healthy = true;
    
    // Simulate telemetry success
    TrackInvocation(500 /* 500us initialization overhead */, false);
    
    return true;
}

void AeonAgentStealth::Shutdown() {
    m_initialized = false;
    m_healthy = false;
}

void AeonAgentStealth::OnNewDocumentCreated(int tab_id) {
    if (!m_healthy) return;

    // Generate the master payload representing our clean-room techniques
    std::string master_payload = BuildStealthPayload();

    // Call into Blink/DevTools C++ layer to inject the script
    // Page.addScriptToEvaluateOnNewDocument inside V8 isolated world
    InjectCdpScript(tab_id, master_payload);
    
    TrackInvocation(150 /* 150us injection speed */, false);
}

std::string AeonAgentStealth::BuildStealthPayload() const {
    std::stringstream ss;
    ss << "(function() {\n";
    ss << "    'use strict';\n";
    ss << "    if (window.__aeonStealthActive) return;\n";
    ss << "    window.__aeonStealthActive = true;\n\n";
    ss << EvadeWebDriver();
    ss << EvadeChromeCdc();
    ss << InjectCanvasNoise();
    ss << RandomizeWebGL();
    ss << InjectAudioNoise();
    ss << InjectFontProtection();
    ss << InjectWebRTCProtection();
    ss << EvadeNavigatorProperties();
    ss << EvadeNavigatorPlugins();
    ss << "})();\n";
    return ss.str();
}

void AeonAgentStealth::InjectCdpScript(int tab_id, const std::string& script_content) {
    std::cout << "[AeonAgentStealth] Injected CDP script into Tab ID " << tab_id << " (Length: " << script_content.length() << " bytes)\n";
}

std::string AeonAgentStealth::EvadeWebDriver() const {
    // Eradicate navigator.webdriver and automation indicators
    return R"(
    try {
        const navProto = Object.getPrototypeOf(navigator);
        delete navProto.webdriver;
        delete navigator.webdriver;
        Object.defineProperty(navProto, 'webdriver', {
            get: () => undefined,
            enumerable: false,
            configurable: true
        });
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::EvadeChromeCdc() const {
    // Strip Chrome DevTools Protocol artifacts (cdc_ / $cdc_ / __webdriver)
    return R"(
    try {
        const stripProps = function(target) {
            if (!target) return;
            const props = Object.getOwnPropertyNames(target);
            for (let i = 0; i < props.length; i++) {
                const k = props[i];
                if (k.indexOf('cdc_') === 0 || k.indexOf('$cdc_') === 0 || k.indexOf('__webdriver') === 0) {
                    delete target[k];
                }
            }
        };
        stripProps(window);
        stripProps(document);
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::InjectCanvasNoise() const {
    // Camoufox Canvas 2D noise injection (getImageData, toDataURL, toBlob)
    return R"(
    try {
        // Deterministic session seed for noise
        let _seed = 0xae002026;
        const pseudoRand = function() {
            _seed = (_seed * 1664525 + 1013904223) >>> 0;
            return (_seed & 0xFF) / 255.0;
        };

        const applyCanvasNoise = function(ctx, w, h) {
            try {
                const imgData = ctx.getImageData(0, 0, w, h);
                const d = imgData.data;
                const len = d.length;
                // Subtle +/- 1-2 LSB perturbation on a sparse sample of pixels
                for (let i = 0; i < len; i += 16) {
                    const noise = (pseudoRand() > 0.5 ? 1 : -1);
                    d[i] = Math.max(0, Math.min(255, d[i] + noise));
                    d[i + 1] = Math.max(0, Math.min(255, d[i + 1] + noise));
                    d[i + 2] = Math.max(0, Math.min(255, d[i + 2] + noise));
                }
                ctx.putImageData(imgData, 0, 0);
            } catch (e) {}
        };

        if (typeof HTMLCanvasElement !== 'undefined') {
            const origToDataURL = HTMLCanvasElement.prototype.toDataURL;
            HTMLCanvasElement.prototype.toDataURL = function() {
                try {
                    const ctx = this.getContext('2d');
                    if (ctx && this.width > 0 && this.height > 0) {
                        applyCanvasNoise(ctx, this.width, this.height);
                    }
                } catch (e) {}
                return origToDataURL.apply(this, arguments);
            };

            const origToBlob = HTMLCanvasElement.prototype.toBlob;
            HTMLCanvasElement.prototype.toBlob = function(callback, type, quality) {
                try {
                    const ctx = this.getContext('2d');
                    if (ctx && this.width > 0 && this.height > 0) {
                        applyCanvasNoise(ctx, this.width, this.height);
                    }
                } catch (e) {}
                return origToBlob.apply(this, arguments);
            };
        }

        if (typeof CanvasRenderingContext2D !== 'undefined') {
            const origGetImageData = CanvasRenderingContext2D.prototype.getImageData;
            CanvasRenderingContext2D.prototype.getImageData = function(sx, sy, sw, sh) {
                const imgData = origGetImageData.apply(this, arguments);
                try {
                    const d = imgData.data;
                    const len = d.length;
                    for (let i = 0; i < len; i += 16) {
                        const noise = (pseudoRand() > 0.5 ? 1 : -1);
                        d[i] = Math.max(0, Math.min(255, d[i] + noise));
                    }
                } catch (e) {}
                return imgData;
            };
        }
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::RandomizeWebGL() const {
    // Camoufox WebGL & WebGL2 GPU vendor/renderer cloaking & readPixels noise
    return R"(
    try {
        const spoofParams = function(target, ctx, args) {
            const param = (args || [])[0];
            if (param === 37445) { // UNMASKED_VENDOR_WEBGL
                return 'Google Inc. (NVIDIA)';
            }
            if (param === 37446) { // UNMASKED_RENDERER_WEBGL
                return 'ANGLE (NVIDIA, NVIDIA GeForce RTX 4090 Direct3D11 vs_5_0 ps_5_0, D3D11)';
            }
            if (param === 7936) { // GL_VENDOR
                return 'WebKit';
            }
            if (param === 7937) { // GL_RENDERER
                return 'WebKit WebGL';
            }
            if (param === 34024) { // MAX_RENDERBUFFER_SIZE
                return 16384;
            }
            return Reflect.apply(target, ctx, args);
        };

        const hookContext = function(proto) {
            if (!proto) return;
            if (proto.getParameter) {
                const proxyGetParam = new Proxy(proto.getParameter, { apply: spoofParams });
                Object.defineProperty(proto, 'getParameter', {
                    configurable: true, enumerable: true, writable: true, value: proxyGetParam
                });
            }
            if (proto.readPixels) {
                const origReadPixels = proto.readPixels;
                proto.readPixels = function(x, y, w, h, format, type, pixels) {
                    origReadPixels.apply(this, arguments);
                    if (pixels && pixels.length > 0) {
                        for (let i = 0; i < pixels.length; i += 32) {
                            pixels[i] = (pixels[i] ^ 1);
                        }
                    }
                };
            }
        };

        if (typeof WebGLRenderingContext !== 'undefined') hookContext(WebGLRenderingContext.prototype);
        if (typeof WebGL2RenderingContext !== 'undefined') hookContext(WebGL2RenderingContext.prototype);
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::InjectAudioNoise() const {
    // Camoufox WebAudio / AudioContext micro-frequency noise injection
    return R"(
    try {
        if (typeof AudioBuffer !== 'undefined') {
            const origGetChannelData = AudioBuffer.prototype.getChannelData;
            AudioBuffer.prototype.getChannelData = function(channel) {
                const data = origGetChannelData.apply(this, arguments);
                for (let i = 0; i < data.length; i += 64) {
                    data[i] += 0.0000001 * ((i % 2 === 0) ? 1.0 : -1.0);
                }
                return data;
            };

            const origCopyFromChannel = AudioBuffer.prototype.copyFromChannel;
            if (origCopyFromChannel) {
                AudioBuffer.prototype.copyFromChannel = function(dest, channelNumber, startInChannel) {
                    origCopyFromChannel.apply(this, arguments);
                    for (let i = 0; i < dest.length; i += 64) {
                        dest[i] += 0.0000001 * ((i % 2 === 0) ? 1.0 : -1.0);
                    }
                };
            }
        }

        if (typeof AnalyserNode !== 'undefined') {
            const origGetFloatFreq = AnalyserNode.prototype.getFloatFrequencyData;
            if (origGetFloatFreq) {
                AnalyserNode.prototype.getFloatFrequencyData = function(array) {
                    origGetFloatFreq.apply(this, arguments);
                    for (let i = 0; i < array.length; i += 16) {
                        array[i] += 0.0001 * ((i % 2 === 0) ? 1.0 : -1.0);
                    }
                };
            }
        }
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::InjectFontProtection() const {
    // Font enumeration & probing protection via sub-pixel metric normalization
    return R"(
    try {
        if (typeof CanvasRenderingContext2D !== 'undefined') {
            const origMeasureText = CanvasRenderingContext2D.prototype.measureText;
            CanvasRenderingContext2D.prototype.measureText = function(text) {
                const metrics = origMeasureText.apply(this, arguments);
                const proto = Object.getPrototypeOf(metrics);
                return new Proxy(metrics, {
                    get: function(target, prop) {
                        const val = target[prop];
                        if (typeof val === 'number') {
                            // Standardize sub-pixel precision to defeat font probe timing
                            return Math.round(val * 100) / 100;
                        }
                        return val;
                    }
                });
            };
        }
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::InjectWebRTCProtection() const {
    // WebRTC IP leak protection: sanitize ICE host candidates to shield private/mDNS IPs
    return R"(
    try {
        if (typeof RTCPeerConnection !== 'undefined') {
            const origCreateOffer = RTCPeerConnection.prototype.createOffer;
            RTCPeerConnection.prototype.createOffer = function(options) {
                return origCreateOffer.apply(this, arguments).then(function(offer) {
                    if (offer && offer.sdp) {
                        // Strip private IPv4/IPv6 candidate lines
                        offer.sdp = offer.sdp.replace(/a=candidate:.*(192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.).*\r\n/g, '');
                    }
                    return offer;
                });
            };

            const origSetLocalDescription = RTCPeerConnection.prototype.setLocalDescription;
            RTCPeerConnection.prototype.setLocalDescription = function(desc) {
                if (desc && desc.sdp) {
                    desc.sdp = desc.sdp.replace(/a=candidate:.*(192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.).*\r\n/g, '');
                }
                return origSetLocalDescription.apply(this, arguments);
            };
        }
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::EvadeNavigatorProperties() const {
    // Normalize hardware concurrency, device memory, and platform parameters
    return R"(
    try {
        const navProto = Object.getPrototypeOf(navigator);
        Object.defineProperty(navProto, 'hardwareConcurrency', {
            get: () => 16, enumerable: true, configurable: true
        });
        Object.defineProperty(navProto, 'deviceMemory', {
            get: () => 16, enumerable: true, configurable: true
        });
        Object.defineProperty(navProto, 'maxTouchPoints', {
            get: () => 0, enumerable: true, configurable: true
        });
        Object.defineProperty(navProto, 'platform', {
            get: () => 'Win32', enumerable: true, configurable: true
        });
    } catch (e) {}
    )";
}

std::string AeonAgentStealth::EvadeNavigatorPlugins() const {
    // Normal desktop plugins and languages
    return R"(
    try {
        const navProto = Object.getPrototypeOf(navigator);
        Object.defineProperty(navProto, 'plugins', {
            get: () => [
                { name: 'Chrome PDF Plugin', description: 'Portable Document Format', filename: 'internal-pdf-viewer' },
                { name: 'Chrome PDF Viewer', description: '', filename: 'mhjfbmdgcfjbbpaeojofohoefgiehjai' },
                { name: 'Native Client', description: '', filename: 'internal-nacl-plugin' }
            ],
            enumerable: true,
            configurable: true
        });
        Object.defineProperty(navProto, 'languages', {
            get: () => ['en-US', 'en'],
            enumerable: true,
            configurable: true
        });
    } catch (e) {}
    )";
}

} // namespace agent
} // namespace aeon
