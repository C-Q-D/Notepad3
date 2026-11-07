; Exercise the sidebar through its real controls and on-disk INI and documents.
; All window input targets handles owned by the isolated executable started here.
#Requires AutoHotkey v2.0
#SingleInstance Off
global testDir := A_Args[1], through := A_Args[2], testPid := 0, main := 0
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
