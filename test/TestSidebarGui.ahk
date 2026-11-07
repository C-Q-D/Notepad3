; Exercise the sidebar through its real controls and on-disk INI and documents.
; All window input targets handles owned by the isolated executable started here.
#Requires AutoHotkey v2.0
#SingleInstance Off
global testDir := A_Args[1], through := A_Args[2], testPid := 0, main := 0
global scenario := A_Args[3]
global logFile := testDir '\gui.log'
exitCode := 0
try {
    Run('"' testDir '\Notepad3.exe" /n "' testDir '\fixture\a.txt"', testDir, , &testPid)
    FileAppend(testPid '`n', testDir '\pids.txt')
    main := WinWait('ahk_pid ' testPid, , 10)
    Check(main != 0, 'A main window starts')
    DllCall('SetWindowPos', 'ptr', main, 'ptr', 0, 'int', 80, 'int', 80, 'int', 1100, 'int', 750, 'uint', 0x14)
    tree := 0
    try tree := ControlGetHwnd('SysTreeView321', main)
    Check(tree != 0 && DllCall('IsWindowVisible', 'ptr', tree), 'A visible SysTreeView32 exists')
    editor := ControlGetHwnd('Scintilla1', main)
    dpi := DllCall('GetDpiForWindow', 'ptr', main, 'uint')
    ControlGetPos(&tx, &ty, &tw, &th, tree)
    Check(Abs(tw - 300 * dpi / 96) <= 4, 'A width is 300 logical pixels (actual ' tw ', DPI ' dpi ')')
    PostMessage(0x111, 41066, 0, , main)
    WaitUntil(() => !DllCall('IsWindowVisible', 'ptr', tree), 'A hide command hides tree')
    ControlGetPos(&ex, , , , editor)
    Check(ex <= 4, 'A hidden sidebar restores editor left edge (actual ' ex ')')
    PostMessage(0x111, 41066, 0, , main)
    WaitUntil(() => DllCall('IsWindowVisible', 'ptr', tree), 'A show command restores tree')
    if Ord(through) >= Ord('B') {
        root := SendMessage(0x110A, 0, 0, tree)
        Check(root != 0, 'B root exists')
        Check(TreeText(tree, root) = 'fixture', 'B root is test directory name')
        child := SendMessage(0x110A, 4, root, tree)
        Check(TreeText(tree, child) = 'sub', 'B folders sort first')
        aItem := SendMessage(0x110A, 1, child, tree)
        Check(TreeText(tree, aItem) = 'a.txt', 'B alphabetical file order')
        bItem := SendMessage(0x110A, 1, aItem, tree)
        Check(TreeText(tree, bItem) = 'b2.txt', 'B natural order b2 before b10')
        last := SendMessage(0x110A, 1, bItem, tree)
        Check(TreeText(tree, last) = 'b10.txt' && SendMessage(0x110A, 1, last, tree) = 0, 'B complete first level order')
        Check(SendMessage(0x110A, 9, 0, tree) = aItem, 'B command line file selected')
        SendMessage(0x1102, 2, child, tree)
        inner := SendMessage(0x110A, 4, child, tree)
        Check(TreeText(tree, inner) = 'inner.txt', 'B expanding sub loads inner.txt')
    }
    if Ord(through) >= Ord('C') && (scenario = 'All' || scenario = 'C') {
        TypeText(editor, 'XYZ')
        WaitUntil(() => SendMessage(2159, 0, 0, editor) != 0, 'C document modified before switch')
        ClickTree(tree, bItem)
        start := A_TickCount
        while A_TickCount - start < 2000 {
            CheckNoDialog('C no save prompt within two seconds')
            Sleep(25)
        }
        Check(true, 'C no save prompt within two seconds')
        Check(InStr(FileRead(testDir '\fixture\a.txt', 'UTF-8'), 'XYZ'), 'C a.txt saved silently before switch')
        Check(InStr(WinGetTitle(main), 'b2.txt'), 'C b2.txt opened')
    }
    if Ord(through) >= Ord('D') && (scenario = 'All' || scenario = 'D') {
        PostMessage(0x111, 40000, 0, , main)
        WaitUntil(() => !InStr(WinGetTitle(main), '.txt'), 'D new untitled document opens')
        TypeText(editor, 'untitled-text')
        WaitUntil(() => SendMessage(2159, 0, 0, editor) != 0, 'D untitled text entered')
        WaitUntil(() => InStr(WinGetTitle(main), '*') != 0, 'D modified title is displayed')
        beforeTitle := WinGetTitle(main)
        beforeText := EditorText(editor)
        ClickTree(tree, TreeFind(tree, 'a.txt'))
        dialog := WinWait('ahk_class #32770 ahk_pid ' testPid, , 3)
        Check(dialog != 0, 'D switching modified untitled document prompts')
        PostMessage(0x111, 2, 0, , dialog)
        Check(WinWaitClose(dialog, , 2), 'D Cancel closes save question')
        Check(WinGetTitle(main) = beforeTitle, 'D Cancel retains untitled title')
        Check(EditorText(editor) = beforeText, 'D Cancel retains editor text')
    }
    if Ord(through) >= Ord('E') && (scenario = 'All' || scenario = 'E') {
        ; Create the read-only fixture only after B's exact four-entry assertion.
        FileAppend('read-only original`r`n', testDir '\fixture\ro.txt', 'UTF-8')
        FileSetAttrib('+R', testDir '\fixture\ro.txt')
        SendMessage(2014, 0, 0, editor)
        PostMessage(0x10, 0, 0, , main)
        Check(WinWaitClose(main, , 5), 'E prior fixture window closes')
        WaitUntil(() => !ProcessExist(testPid), 'E prior fixture process exits', 5000)
        Run('"' testDir '\Notepad3.exe" /n "' testDir '\fixture\ro.txt"', testDir, , &testPid)
        FileAppend(testPid '`n', testDir '\pids.txt')
        main := WinWait('ahk_pid ' testPid, , 10)
        Check(main != 0, 'E read-only fixture window starts')
        tree := ControlGetHwnd('SysTreeView321', main)
        editor := ControlGetHwnd('Scintilla1', main)
        WaitUntil(() => TreeFind(tree, 'b2.txt') != 0, 'E root restored')
        TypeText(editor, 'readonly-change')
        WaitUntil(() => SendMessage(2159, 0, 0, editor) != 0, 'E read-only attribute file can be edited')
        ClickTree(tree, TreeFind(tree, 'b2.txt'))
        dialog := WinWait('ahk_class #32770 ahk_pid ' testPid, , 3)
        Check(dialog != 0, 'E existing read-only save prompt appears')
        Check(InStr(WinGetText(dialog), 'different file'), 'E prompt offers saving to a different file')
        PostMessage(0x111, 7, 0, , dialog)
        Check(WinWaitClose(dialog, , 2), 'E No closes read-only prompt')
        Check(InStr(WinGetTitle(main), 'ro.txt'), 'E No keeps ro.txt open')
    }
    ; Clear the dirty flag only after all assertions, so normal close tests settings.
    SendMessage(2014, 0, 0, editor)
    PostMessage(0x10, 0, 0, , main)
    Check(WinWaitClose(main, , 5), 'A normal close completes')
    WaitUntil(() => !ProcessExist(testPid), 'A process exits after settings save', 5000)
    main := 0
    ini := testDir '\Notepad3.ini'
    visible := ReadSetting(ini, 'ShowSidebar')
    Check(visible = '1' || visible = 'true', 'A visibility persisted')
    Check(ReadSetting(ini, 'SidebarWidth') = '300', 'A width persisted')
    folder := ReadSetting(ini, 'SidebarFolder')
    expanded := Buffer(65536, 0)
    DllCall('ExpandEnvironmentStringsW', 'str', folder, 'ptr', expanded, 'uint', 32768)
    folder := StrGet(expanded)
    full := Buffer(65536, 0)
    DllCall('GetFullPathNameW', 'str', testDir '\' folder, 'uint', 32768, 'ptr', full, 'ptr', 0)
    ; Absolute and relative paths are both valid under the application's path policy.
    Check(StrLower(folder) = StrLower(testDir '\fixture') || StrLower(StrGet(full)) = StrLower(testDir '\fixture'), 'A folder persisted')
} catch Error as err {
    FileAppend('FAIL: ' err.Message '`n', logFile)
    exitCode := 1
} finally {
    if main && WinExist(main) {
        ; Close only this test instance. The PowerShell owner enforces final cleanup.
        try PostMessage(0x10, 0, 0, , main)
    }
}
ExitApp(exitCode)

; Post character messages to the specified editor without touching desktop input.
TypeText(editor, text) {
    ControlFocus(editor)
    Loop Parse text
        PostMessage(0x102, Ord(A_LoopField), 0, editor)
}

; Reject any modal dialog owned by our process during the silent-save interval.
CheckNoDialog(name) {
    global testPid
    if WinExist('ahk_class #32770 ahk_pid ' testPid)
        Check(false, name)
}

; Find a named direct root child; zero means absent, not an inferred selection.
TreeFind(tree, name) {
    root := SendMessage(0x110A, 0, 0, tree)
    item := SendMessage(0x110A, 4, root, tree)
    while item {
        if TreeText(tree, item) = name
            return item
        item := SendMessage(0x110A, 1, item, tree)
    }
    return 0
}

; Click the real item's text rectangle using client-coordinate mouse messages.
; The pointer stays on the user's desktop position throughout this operation.
ClickTree(tree, item) {
    global testPid
    Check(item != 0, 'target tree item exists')
    SendMessage(0x1114, 0, item, tree)
    process := DllCall('OpenProcess', 'uint', 0x438, 'int', false, 'uint', testPid, 'ptr')
    memory := DllCall('VirtualAllocEx', 'ptr', process, 'ptr', 0, 'uptr', 64, 'uint', 0x3000, 'uint', 4, 'ptr')
    try {
        data := Buffer(64, 0)
        NumPut('ptr', item, data)
        DllCall('WriteProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
        Check(SendMessage(0x1104, 1, memory, tree), 'target tree rectangle available')
        DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
        x := NumGet(data, 0, 'int') + 4
        y := (NumGet(data, 4, 'int') + NumGet(data, 12, 'int')) // 2
        point := (y << 16) | (x & 0xffff)
        PostMessage(0x201, 1, point, tree)
        PostMessage(0x202, 0, point, tree)
    } finally {
        if memory
            DllCall('VirtualFreeEx', 'ptr', process, 'ptr', memory, 'uptr', 0, 'uint', 0x8000)
        if process
            DllCall('CloseHandle', 'ptr', process)
    }
}

; Read editor bytes through SCI_GETTEXT in its process; return decoded UTF-8 text.
EditorText(editor) {
    global testPid
    length := SendMessage(2183, 0, 0, editor) + 1
    process := DllCall('OpenProcess', 'uint', 0x438, 'int', false, 'uint', testPid, 'ptr')
    memory := DllCall('VirtualAllocEx', 'ptr', process, 'ptr', 0, 'uptr', length, 'uint', 0x3000, 'uint', 4, 'ptr')
    try {
        if !process || !memory
            throw Error('Cannot allocate remote editor buffer')
        SendMessage(2182, length, memory, editor)
        data := Buffer(length, 0)
        DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', length, 'ptr', 0)
        return StrGet(data, 'UTF-8')
    } finally {
        if memory
            DllCall('VirtualFreeEx', 'ptr', process, 'ptr', memory, 'uptr', 0, 'uint', 0x8000)
        if process
            DllCall('CloseHandle', 'ptr', process)
    }
}

; Read a real remote TVITEMW. Native common controls require target-process buffers.
TreeText(tree, item) {
    global testPid
    if !item
        return ''
    process := DllCall('OpenProcess', 'uint', 0x438, 'int', false, 'uint', testPid, 'ptr')
    memory := DllCall('VirtualAllocEx', 'ptr', process, 'ptr', 0, 'uptr', 4096, 'uint', 0x3000, 'uint', 4, 'ptr')
    try {
        if !process || !memory
            throw Error('Cannot allocate remote TreeView buffer')
        data := Buffer(4096, 0)
        NumPut('uint', 1, data, 0)
        NumPut('ptr', item, data, 8)
        NumPut('ptr', memory + 128, data, 24)
        NumPut('int', 256, data, 32)
        DllCall('WriteProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 4096, 'ptr', 0)
        if !SendMessage(0x113E, 0, memory, tree)
            throw Error('TVM_GETITEMW failed')
        DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 4096, 'ptr', 0)
        return StrGet(data.Ptr + 128, 'UTF-16')
    } finally {
        if memory
            DllCall('VirtualFreeEx', 'ptr', process, 'ptr', memory, 'uptr', 0, 'uint', 0x8000)
        if process
            DllCall('CloseHandle', 'ptr', process)
    }
}

; Read the UTF-8 INI written by SimpleIni. Win32 IniRead assumes ANSI or UTF-16.
ReadSetting(path, key) {
    text := FileRead(path, 'UTF-8')
    if !RegExMatch(text, 'ms)^\[Settings\]\R(.*?)(?:^\[|\z)', &section)
        throw Error('Missing Settings section')
    if !RegExMatch(section[1], 'm)^' key '=([^\r\n]*)', &value)
        throw Error('Missing Settings key: ' key)
    return value[1]
}

; Record an independently specified assertion and stop at its first failure.
Check(pass, name) {
    global logFile
    FileAppend((pass ? 'PASS: ' : 'FAIL: ') name '`n', logFile)
    if !pass
        throw Error(name)
}

; Poll an observable condition for at most timeoutMs, then report its assertion.
WaitUntil(condition, name, timeoutMs := 3000) {
    start := A_TickCount
    while !condition.Call() && A_TickCount - start < timeoutMs
        Sleep(25)
    Check(condition.Call(), name)
}
