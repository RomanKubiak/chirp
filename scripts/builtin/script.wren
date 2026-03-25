foreign class ScriptNative {
    foreign static loadingName()
    foreign static enterContext(name)
    foreign static leaveContext()
    foreign static canDraw()
    foreign static activeDisplayScript()
    foreign static nextDisplayScript()
    foreign static prevDisplayScript()
    foreign static selectDisplayScript(name)
}

class Script {
    static wrapListener(callback) {
        var owner = ScriptNative.loadingName()
        return {
            "owner": owner,
            "fn": callback
        }
    }

    static canDraw { ScriptNative.canDraw() }

    static onUnload(fn) { __fn = fn }
    static callUnload() {
        if (__fn is Fn) __fn.call()
        __fn = null
    }

    static onFocus(fn) { __focusFn = fn }
    static callFocus() {
        if (__focusFn is Fn) __focusFn.call()
    }
}

Script.onUnload(null)
Script.onFocus(null)