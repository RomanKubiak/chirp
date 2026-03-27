foreign class ScriptNative {
    foreign static loadingName()
    foreign static enterContext(name)
    foreign static leaveContext()
}

class Script {
    static loadingName() {
        return ScriptNative.loadingName()
    }

    static wrapListener(callback) {
        var owner = ScriptNative.loadingName()
        return {
            "owner": owner,
            "fn": callback
        }
    }

    static onUnload(fn) { 
        if (!__unloadCallbacks) __unloadCallbacks = {}
        var scriptName = ScriptNative.loadingName()
        __unloadCallbacks[scriptName] = fn
    }
    
    static callUnload() {
        if (!__unloadCallbacks) __unloadCallbacks = {}
        var scriptName = ScriptNative.loadingName()
        var fn = __unloadCallbacks[scriptName]
        if (fn is Fn) fn.call()
        __unloadCallbacks[scriptName] = null
        
        if (!__focusCallbacks) __focusCallbacks = {}
        __focusCallbacks[scriptName] = null  // clear focus callback so old module closures are fully released
    }

    static onFocus(fn) {
        if (!__focusCallbacks) __focusCallbacks = {}
        var scriptName = ScriptNative.loadingName()
        __focusCallbacks[scriptName] = fn
    }
    
    static callFocus() {
        if (!__focusCallbacks) __focusCallbacks = {}
        var scriptName = ScriptNative.loadingName()
        var fn = __focusCallbacks[scriptName]
        if (fn is Fn) fn.call()
    }
}

// Initialize callback maps if not already present
if (!__unloadCallbacks) __unloadCallbacks = {}
if (!__focusCallbacks) __focusCallbacks = {}