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
}

Script.onUnload(null)