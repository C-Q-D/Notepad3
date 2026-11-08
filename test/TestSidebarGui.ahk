; Exercise the sidebar through its real controls and on-disk INI and documents.
; All window input targets handles owned by the isolated executable started here.
#Requires AutoHotkey v2.0
#SingleInstance Off
global testDir := A_Args[1], through := A_Args[2], testPid := 0, testCreated := 0, main := 0
global scenario := A_Args[3]
global screenshotDirectory := A_Args[4], themeName := A_Args[5]
global logFile := testDir '\gui.log'
exitCode := 0
try {
    Run('"' testDir '\Notepad3.exe" /n "' testDir '\fixture\a.txt"', testDir, , &testPid)
    testCreated := RecordProcess(testPid)
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
    ; Check startup before Show/Refresh can populate or repair an empty tree.
    if Ord(through) >= Ord('B') {
        WaitUntil(() => SendMessage(0x110A, 0, 0, tree) != 0, 'B startup root restored before any toggle')
        root := SendMessage(0x110A, 0, 0, tree)
        Check(TreeText(tree, root) = 'fixture', 'B startup root is test directory name')
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'a.txt', 'B startup command line file selected before any toggle')
    }
    DragWidth(tree, 50)
    ControlGetPos(, , &tw, , tree)
    Check(Abs(tw - 100 * dpi / 96) <= 4, 'A dragging enforces minimum width')
    DragWidth(tree, 4000)
    ControlGetPos(, , &editorWidth, , editor)
    Check(editorWidth >= 200 * dpi / 96 - 4, 'A dragging reserves editor width')
    DragWidth(tree, 300)
    ControlGetPos(, , &tw, , tree)
    Check(Abs(tw - 300 * dpi / 96) <= 4, 'A dragging restores requested width')
    PostMessage(0x111, 41066, 0, , main)
    WaitUntil(() => !DllCall('IsWindowVisible', 'ptr', tree), 'A hide command hides tree')
    WaitUntil(() => ControlLeft(editor) <= 4, 'A hidden sidebar restores editor left edge')
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
        ClickTree(tree, child, true)
        WaitUntil(() => SendMessage(0x110A, 4, child, tree) != 0, 'B expand button loads sub')
        inner := SendMessage(0x110A, 4, child, tree)
        Check(TreeText(tree, inner) = 'inner.txt', 'B expanding sub loads inner.txt')
        ClickTree(tree, child, true)
        WaitUntil(() => SendMessage(0x110A, 4, child, tree) = 0, 'B collapsing sub discards loaded children')
        ClickTree(tree, child, true)
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 4, child, tree)) = 'inner.txt', 'B re-expanding sub reloads contents')
        Loop 3
            SendMessage(0x31A, 0, 0, , main)
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'a.txt', 'B theme updates preserve current-file selection')
    }
    if screenshotDirectory != '' {
        CaptureWindow(main, screenshotDirectory '\sidebar-' StrLower(themeName) '.png')
        Check(true, 'window screenshot saved for ' themeName ' theme')
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
        WaitUntil(() => InStr(WinGetTitle(main), 'b2.txt') != 0, 'C b2.txt opened')
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
        WaitUntil(() => WinGetTitle(main) = beforeTitle, 'D Cancel retains untitled title')
        WaitUntil(() => EditorText(editor) = beforeText, 'D Cancel retains editor text')
        WaitUntil(() => SendMessage(0x110A, 9, 0, tree) = 0, 'D Cancel clears tree selection for untitled document')
    }
    if Ord(through) >= Ord('E') && (scenario = 'All' || scenario = 'E') {
        ; Create the read-only fixture only after B's exact four-entry assertion.
        FileAppend('read-only original`r`n', testDir '\fixture\ro.txt', 'UTF-8')
        FileSetAttrib('+R', testDir '\fixture\ro.txt')
        SendMessage(2014, 0, 0, editor)
        CloseTestWindow(main)
        Check(WinWaitClose(main, , 5), 'E prior fixture window closes')
        WaitUntil(() => !ProcessExist(testPid), 'E prior fixture process exits', 5000)
        Run('"' testDir '\Notepad3.exe" /n "' testDir '\fixture\ro.txt"', testDir, , &testPid)
        testCreated := RecordProcess(testPid)
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
        WaitUntil(() => InStr(WinGetTitle(main), 'ro.txt') != 0, 'E No keeps ro.txt open')
        ; Destroying the prompt precedes the caller's current-file synchronization.
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'ro.txt', 'E No restores real current-file selection')
    }
    if Ord(through) >= Ord('F') && (scenario = 'All' || scenario = 'F') {
        SendMessage(2014, 0, 0, editor)
        ClickTree(tree, TreeFind(tree, 'a.txt'))
        WaitUntil(() => InStr(WinGetTitle(main), 'a.txt') != 0, 'F clean starting file opens')
        bItem := TreeFind(tree, 'b2.txt')
        SendMessage(0x110B, 9, bItem, tree)
        ControlFocus(tree)
        TreeKey(tree, 0x0D)
        WaitUntil(() => InStr(WinGetTitle(main), 'b2.txt') != 0, 'F Enter opens selected b2.txt')
        Check(SendMessage(2159, 0, 0, editor) = 0, 'F Enter does not modify document')
        WaitUntil(() => !InStr(WinGetTitle(main), '*'), 'F Enter title has no modified marker')
        Check(SendMessage(2154, 0, 0, editor) = 2, 'F Enter keeps two fixture lines')
        TypeText(editor, 'keep-F5-change')
        WaitUntil(() => SendMessage(2159, 0, 0, editor) != 0, 'F edit before F5 is present')
        beforeText := EditorText(editor)
        subItem := TreeFind(tree, 'sub')
        SendMessage(0x1102, 2, subItem, tree)
        FileAppend('refresh fixture', testDir '\fixture\refresh.txt', 'UTF-8')
        ControlFocus(tree)
        TreeKey(tree, 0x74)
        WaitUntil(() => TreeFind(tree, 'refresh.txt') != 0, 'F F5 re-enumerates tree')
        CheckNoDialog('F F5 does not invoke file revert prompt')
        Check(true, 'F F5 does not invoke file revert prompt')
        Check(EditorText(editor) = beforeText, 'F F5 preserves unsaved editor text')
        Check(TreeFind(tree, 'a.txt') != 0 && TreeFind(tree, 'b2.txt') != 0 && TreeFind(tree, 'b10.txt') != 0, 'F F5 retains original tree entries')
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'b2.txt', 'F F5 restores selection')
        subItem := TreeFind(tree, 'sub')
        WaitUntil(() => (SendMessage(0x1127, subItem, 0x20, tree) & 0x20) != 0 && TreeText(tree, SendMessage(0x110A, 4, subItem, tree)) = 'inner.txt', 'F F5 restores expanded directory')
        SendMessage(0x110B, 9, subItem, tree)
        ControlFocus(tree)
        TreeKey(tree, 0x0D)
        WaitUntil(() => SendMessage(0x110A, 4, subItem, tree) = 0, 'F Enter collapses directory and discards children')
        TreeKey(tree, 0x0D)
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 4, subItem, tree)) = 'inner.txt', 'F Enter re-expands and reloads directory')
        Check(EditorText(editor) = beforeText, 'F directory Enter leaves editor text unchanged')
        ControlFocus(tree)
        TreeKey(tree, 0x1B)
        WaitUntil(() => ControlGetFocus(main) = editor, 'F Esc returns focus to editor')
        ControlFocus(tree)
        TreeKey(tree, 0x09)
        WaitUntil(() => ControlGetFocus(main) = editor, 'F Tab returns focus to editor')
    }
    if Ord(through) >= Ord('F') && (scenario = 'All' || scenario = 'LongPath') {
        ; Create this extra branch after B's exact four-entry assertion.
        SendMessage(2014, 0, 0, editor)
        longDir := testDir '\fixture\long-path'
        segment := 'segment-'
        Loop 72
            segment .= 'x'
        Loop 3
            longDir .= '\' segment
        Check(StrLen(longDir) > 260, 'LongPath expanded directory crosses 260 characters (actual ' StrLen(longDir) ')')
        DirCreate('\\?\' longDir)
        FileAppend('long-path document', '\\?\' longDir '\long.txt', 'UTF-8')
        ControlFocus(tree)
        TreeKey(tree, 0x74)
        WaitUntil(() => TreeFind(tree, 'long-path') != 0, 'LongPath refresh finds new branch')
        longParent := TreeFind(tree, 'long-path')
        SendMessage(0x1102, 2, longParent, tree)
        Loop 3 {
            longParent := TreeFind(tree, segment, longParent)
            Check(longParent != 0, 'LongPath directory component exists')
            SendMessage(0x1102, 2, longParent, tree)
        }
        longItem := TreeFind(tree, 'long.txt', longParent)
        ClickTree(tree, longItem)
        WaitUntil(() => InStr(WinGetTitle(main), 'long.txt') != 0, 'LongPath file opens')
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'long.txt', 'LongPath current file remains selected')
        FileAppend('refresh completed', '\\?\' longDir '\refresh-long.txt', 'UTF-8')
        ControlFocus(tree)
        TreeKey(tree, 0x74)
        ; A pre-refresh selection already matches. A newly created child proves
        ; the posted F5 has actually rebuilt the tree before checking restoration.
        WaitUntil(() => LongPathRefreshItem(tree, segment) != 0, 'LongPath F5 enumerates newly created child')
        WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = 'long.txt', 'LongPath F5 restores selected file')
        longParent := TreeFind(tree, 'long-path')
        Loop 3 {
            longParent := TreeFind(tree, segment, longParent)
            Check(longParent != 0 && (SendMessage(0x1127, longParent, 0x20, tree) & 0x20), 'LongPath F5 restores expanded directory')
        }
        Check(EditorText(editor) = 'long-path document', 'LongPath refresh preserves editor text')
    }
    ; Clear the dirty flag only after all assertions, so normal close tests settings.
    SendMessage(2014, 0, 0, editor)
    CloseTestWindow(main)
    Check(WinWaitClose(main, , 5), 'A normal close completes')
    WaitUntil(() => !ProcessExist(testPid), 'A process exits after settings save', 5000)
    main := 0
    ini := testDir '\Notepad3.ini'
    visible := ReadSetting(ini, 'ShowSidebar')
    Check(visible = '1' || visible = 'true', 'A visibility persisted')
    ; The 300 default may be omitted; restart must still restore the actual width.
    Check(ReadSetting(ini, 'SidebarWidth', '300') = '300', 'A width persisted')
    Check(StrLower(PersistedSidebarFolder()) = StrLower(testDir '\fixture'), 'A folder persisted')
    Run('"' testDir '\Notepad3.exe" /n "' testDir '\fixture\a.txt"', testDir, , &testPid)
    testCreated := RecordProcess(testPid)
    main := WinWait('ahk_pid ' testPid, , 10)
    Check(main != 0, 'A restart with saved settings starts')
    DllCall('SetWindowPos', 'ptr', main, 'ptr', 0, 'int', 80, 'int', 80, 'int', 1100, 'int', 750, 'uint', 0x14)
    tree := ControlGetHwnd('SysTreeView321', main)
    dpi := DllCall('GetDpiForWindow', 'ptr', main, 'uint')
    ControlGetPos(, , &tw, , tree)
    Check(Abs(tw - 300 * dpi / 96) <= 4, 'A restart restores 300 logical pixel width (actual ' tw ', DPI ' dpi ')')
    FinishSidebarScenario('A restart')
    if Ord(through) >= Ord('F') && (scenario = 'All' || scenario = 'ShortPath')
        RunShortPathScenario()
    if Ord(through) >= Ord('F') && (scenario = 'All' || scenario = 'OpenFolder')
        RunOpenFolderScenario()
} catch Error as err {
    FileAppend('FAIL: ' err.Message '`n', logFile)
    exitCode := 1
} finally {
    if main && WinExist(main) {
        ; Close only this test instance. The PowerShell owner enforces final cleanup.
        try CloseTestWindow(main)
    }
}
ExitApp(exitCode)

; Resolve the deep refresh marker using fresh tree handles on every poll, since
; F5 invalidates the prior generation's item handles while rebuilding the tree.
LongPathRefreshItem(tree, segment) {
    parent := TreeFind(tree, 'long-path')
    Loop 3 {
        if !parent
            return 0
        parent := TreeFind(tree, segment, parent)
    }
    return parent ? TreeFind(tree, 'refresh-long.txt', parent) : 0
}

; Start a fresh, recorded instance with explicit sidebar settings, without repairing
; its tree through a visibility toggle. folder may be empty or an 8.3 alias;
; visible is 0/1 and file is an existing absolute path opened without editing.
; Return the tree handle; failed startup/assertions throw to the owner's cleanup.
StartSidebarScenario(folder, visible, file) {
    global testDir, testPid, testCreated, main, themeName
    ini := testDir '\Notepad3.ini'
    FileDelete(ini)
    FileAppend('[Settings]`r`nSettingsVersion=5`r`nSaveSettings=1`r`nSaveRecentFiles=0`r`n'
        'ShowSidebar=' visible '`r`nSidebarWidth=300`r`nSidebarFolder=' folder '`r`n'
        'ReuseWindow=0`r`nSingleFileInstance=0`r`nWinThemeDarkMode=' (themeName = 'Dark' ? 1 : 2)
        '`r`n[Settings2]`r`nPreferredLanguageLocaleName=en-US`r`n', ini, 'UTF-8')
    Run('"' testDir '\Notepad3.exe" /n "' file '"', testDir, , &testPid)
    testCreated := RecordProcess(testPid)
    main := WinWait('ahk_pid ' testPid, , 10)
    Check(main != 0, 'scenario instance starts')
    return ControlGetHwnd('SysTreeView321', main)
}

; Wait for both window and process exit so the next instance sees saved settings.
FinishSidebarScenario(label) {
    global main, testPid
    CloseTestWindow(main)
    Check(WinWaitClose(main, , 5), label ' normal close completes')
    WaitUntil(() => !ProcessExist(testPid), label ' process exits after settings save', 5000)
    main := 0
}

; Exercise the public root entry with an actual filesystem-provided 8.3 alias.
; An unavailable alias is reported as a skip, never as a passing regression.
RunShortPathScenario() {
    global testDir, main, logFile
    folder := testDir '\Long Named Sidebar Folder'
    DirCreate(folder)
    FileAppend('short-root first', folder '\a.txt', 'UTF-8')
    FileAppend('short-root second', folder '\b.txt', 'UTF-8')
    shortBuffer := Buffer(65536, 0)
    length := DllCall('GetShortPathNameW', 'str', folder, 'ptr', shortBuffer, 'uint', 32768, 'uint')
    shortFolder := length && length < 32768 ? StrGet(shortBuffer) : ''
    if shortFolder = '' || StrLower(shortFolder) = StrLower(folder) {
        FileAppend('SKIP: ShortPath filesystem supplied no distinct 8.3 alias (8.3 name generation may be disabled)`n', logFile)
        ; Existing aliases can still reproduce the bug when new 8.3 names are
        ; disabled. Use the installed runner's directory strictly for reading.
        folder := RegExReplace(A_AhkPath, '\\v2\\[^\\]+$', '')
        length := DllCall('GetShortPathNameW', 'str', folder, 'ptr', shortBuffer, 'uint', 32768, 'uint')
        shortFolder := length && length < 32768 ? StrGet(shortBuffer) : ''
        if shortFolder = '' || StrLower(shortFolder) = StrLower(folder) || !FileExist(folder '\license.txt') || !FileExist(folder '\WindowSpy.ahk') {
            FileAppend('SKIP: ShortPath no existing runner-directory alias with readable fixture files`n', logFile)
            return
        }
        firstFile := 'license.txt', secondFile := 'WindowSpy.ahk'
        FileAppend('INFO: ShortPath uses existing read-only runner directory alias ' shortFolder '`n', logFile)
    } else {
        firstFile := 'a.txt', secondFile := 'b.txt'
    }
    Check(true, 'ShortPath filesystem provides a distinct 8.3 root alias')
    tree := StartSidebarScenario(shortFolder, 1, folder '\' firstFile)
    WaitUntil(() => SendMessage(0x110A, 0, 0, tree) != 0, 'ShortPath startup root exists before any toggle')
    WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = firstFile, 'ShortPath startup selects long-name current file')
    SplitPath(folder, &folderName)
    Check(TreeText(tree, SendMessage(0x110A, 0, 0, tree)) = folderName, 'ShortPath root uses expanded long name')
    ClickTree(tree, TreeFind(tree, secondFile))
    WaitUntil(() => InStr(WinGetTitle(main), secondFile) != 0, 'ShortPath clicking opens second file')
    WaitUntil(() => TreeText(tree, SendMessage(0x110A, 9, 0, tree)) = secondFile, 'ShortPath click synchronizes long-name current file')
    FinishSidebarScenario('ShortPath')
}

; Open the real localized folder picker via its menu command. The dialog owner,
; title and editable path control must belong to this recorded test instance.
OpenSidebarFolderDialog() {
    global main, testPid
    PostMessage(0x111, 40028, 0, , main)
    dialog := WinWait('ahk_class #32770 ahk_pid ' testPid, , 5)
    Check(dialog != 0, 'OpenFolder directory picker appears')
    Check(WinGetTitle(dialog) = 'Select folder to show in the sidebar', 'OpenFolder picker has exact English title')
    return dialog
}

; Cover confirm and cancel through the real directory dialog, including the
; hidden/empty starting state, checked View menu and settings written on exit.
RunOpenFolderScenario() {
    global testDir, main
    tree := StartSidebarScenario('', 0, testDir '\fixture\a.txt')
    WaitUntil(() => !DllCall('IsWindowVisible', 'ptr', tree), 'OpenFolder starts with hidden sidebar')
    Check(SendMessage(0x110A, 0, 0, tree) = 0, 'OpenFolder starts without a root')
    dialog := OpenSidebarFolderDialog()
    PostMessage(0x111, 2, 0, , dialog)
    Check(WinWaitClose(dialog, , 3), 'OpenFolder hidden-state Cancel closes picker')
    WaitUntil(() => !DllCall('IsWindowVisible', 'ptr', tree) && SendMessage(0x110A, 0, 0, tree) = 0, 'OpenFolder Cancel preserves hidden empty sidebar')
    dialog := OpenSidebarFolderDialog()
    edit := ControlGetHwnd('Edit1', dialog)
    ControlSetText(testDir '\fixture', edit)
    ; BM_CLICK targets the actual shell button; a synthetic WM_COMMAND without
    ; its control handle does not reliably invoke IFileDialog's confirmation.
    SendMessage(0xF5, 0, 0, ControlGetHwnd('Button1', dialog))
    Check(WinWaitClose(dialog, , 5), 'OpenFolder path confirmation closes picker')
    WaitUntil(() => DllCall('IsWindowVisible', 'ptr', tree), 'OpenFolder confirm shows sidebar')
    WaitUntil(() => TreeText(tree, SendMessage(0x110A, 0, 0, tree)) = 'fixture', 'OpenFolder confirm sets selected directory root')
    menu := DllCall('GetMenu', 'ptr', main, 'ptr')
    SendMessage(0x116, menu, 0, , main)
    menuState := 0
    WaitUntil(() => (menuState := DllCall('GetMenuState', 'ptr', menu, 'uint', 41066, 'uint', 0, 'uint')) != 0xffffffff && (menuState & 8) != 0, 'OpenFolder View sidebar menu is checked')
    root := SendMessage(0x110A, 0, 0, tree)
    dialog := OpenSidebarFolderDialog()
    PostMessage(0x111, 2, 0, , dialog)
    Check(WinWaitClose(dialog, , 3), 'OpenFolder visible-state Cancel closes picker')
    WaitUntil(() => DllCall('IsWindowVisible', 'ptr', tree) && SendMessage(0x110A, 0, 0, tree) = root, 'OpenFolder Cancel preserves visible root')
    FinishSidebarScenario('OpenFolder')
    visible := ReadSetting(testDir '\Notepad3.ini', 'ShowSidebar')
    Check(visible = '1' || visible = 'true', 'OpenFolder visibility persisted')
    Check(StrLower(PersistedSidebarFolder()) = StrLower(testDir '\fixture'), 'OpenFolder selected directory persisted')
}

; Return a control's parent-client left edge for bounded asynchronous layout checks.
ControlLeft(control) {
    ControlGetPos(&left, , , , control)
    return left
}

; Resolve the documented environment-variable/relative INI representation before
; comparing it with a fixture's absolute path. This does not consult the tree.
PersistedSidebarFolder() {
    global testDir
    folder := ReadSetting(testDir '\Notepad3.ini', 'SidebarFolder')
    expanded := Buffer(65536, 0)
    if !DllCall('ExpandEnvironmentStringsW', 'str', folder, 'ptr', expanded, 'uint', 32768)
        throw Error('Cannot expand persisted sidebar folder')
    folder := StrGet(expanded)
    if !RegExMatch(folder, 'i)^(?:[a-z]:\\|\\\\)')
        folder := testDir '\' folder
    full := Buffer(65536, 0)
    if !DllCall('GetFullPathNameW', 'str', folder, 'uint', 32768, 'ptr', full, 'ptr', 0)
        throw Error('Cannot resolve persisted sidebar folder')
    return StrGet(full)
}

; Return the OS creation FILETIME for pid, or zero if that process has exited.
; A creation timestamp prevents a recycled PID from authorizing window cleanup.
ProcessCreationTime(pid) {
    process := DllCall('OpenProcess', 'uint', 0x1000, 'int', false, 'uint', pid, 'ptr')
    if !process
        return 0
    try {
        times := Buffer(32, 0)
        if !DllCall('GetProcessTimes', 'ptr', process, 'ptr', times, 'ptr', times.Ptr + 8, 'ptr', times.Ptr + 16, 'ptr', times.Ptr + 24)
            throw Error('Cannot read test process creation time')
        return NumGet(times, 0, 'int64')
    } finally {
        DllCall('CloseHandle', 'ptr', process)
    }
}

; Record both identity fields immediately after starting a Notepad3 test instance.
; PowerShell uses the same FILETIME value before closing or killing that process.
RecordProcess(pid) {
    global testDir
    created := ProcessCreationTime(pid)
    if !created
        throw Error('Test process exited before ownership could be recorded')
    FileAppend(pid '|' created '`n', testDir '\pids.txt')
    return created
}

; Close only the window whose process still matches this test's recorded identity.
CloseTestWindow(window) {
    global testPid, testCreated
    if WinGetPID(window) != testPid || ProcessCreationTime(testPid) != testCreated
        throw Error('Test process identity changed before close')
    PostMessage(0x10, 0, 0, , window)
}

; Exercise the actual splitter's capture/drag path with client-coordinate messages.
; logicalWidth is the desired left-pane width in 96-DPI logical pixels.
DragWidth(tree, logicalWidth) {
    parent := DllCall('GetParent', 'ptr', tree, 'ptr')
    split := DllCall('GetDlgItem', 'ptr', parent, 'int', 0xFB0A, 'ptr')
    dpi := DllCall('GetDpiForWindow', 'ptr', parent, 'uint')
    ControlGetPos(&left, , , , split)
    point := (Round(logicalWidth * dpi / 96) - left) & 0xffff
    SendMessage(0x201, 1, 0, split)
    SendMessage(0x200, 1, point, split)
    SendMessage(0x202, 0, 0, split)
}

; Capture only the test window with PrintWindow and encode its bitmap as PNG.
; All GDI/GDI+ handles are released even when capture or encoding fails.
CaptureWindow(window, path) {
    ; Theme/layout messages invalidate children; paint them before PrintWindow's snapshot.
    DllCall('RedrawWindow', 'ptr', window, 'ptr', 0, 'ptr', 0, 'uint', 0x185)
    WinGetPos(, , &width, &height, window)
    screen := DllCall('GetDC', 'ptr', window, 'ptr')
    canvas := DllCall('CreateCompatibleDC', 'ptr', screen, 'ptr')
    bitmap := DllCall('CreateCompatibleBitmap', 'ptr', screen, 'int', width, 'int', height, 'ptr')
    previous := DllCall('SelectObject', 'ptr', canvas, 'ptr', bitmap, 'ptr')
    gdipBitmap := 0, token := 0
    library := DllCall('LoadLibraryW', 'str', 'gdiplus.dll', 'ptr')
    try {
        if !screen || !canvas || !bitmap || !DllCall('PrintWindow', 'ptr', window, 'ptr', canvas, 'uint', 2)
            throw Error('PrintWindow capture failed')
        startup := Buffer(24, 0)
        NumPut('uint', 1, startup)
        if DllCall('gdiplus\GdiplusStartup', 'uptr*', &token, 'ptr', startup, 'ptr', 0)
            throw Error('GDI+ startup failed')
        if DllCall('gdiplus\GdipCreateBitmapFromHBITMAP', 'ptr', bitmap, 'ptr', 0, 'ptr*', &gdipBitmap)
            throw Error('GDI+ bitmap conversion failed')
        encoder := Buffer(16, 0)
        DllCall('ole32\CLSIDFromString', 'str', '{557CF406-1A04-11D3-9A73-0000F81EF32E}', 'ptr', encoder)
        if DllCall('gdiplus\GdipSaveImageToFile', 'ptr', gdipBitmap, 'wstr', path, 'ptr', encoder, 'ptr', 0)
            throw Error('PNG encoding failed')
    } finally {
        if gdipBitmap
            DllCall('gdiplus\GdipDisposeImage', 'ptr', gdipBitmap)
        if token
            DllCall('gdiplus\GdiplusShutdown', 'uptr', token)
        if previous
            DllCall('SelectObject', 'ptr', canvas, 'ptr', previous)
        if bitmap
            DllCall('DeleteObject', 'ptr', bitmap)
        if canvas
            DllCall('DeleteDC', 'ptr', canvas)
        if screen
            DllCall('ReleaseDC', 'ptr', window, 'ptr', screen)
        if library
            DllCall('FreeLibrary', 'ptr', library)
    }
}

; Send key transitions only to this tree; accelerator handling is exercised normally.
TreeKey(tree, virtualKey) {
    PostMessage(0x100, virtualKey, 0, tree)
    PostMessage(0x101, virtualKey, 0, tree)
}

; Send character messages synchronously so snapshots include the full test input.
; The specified editor receives them without any global keyboard input.
TypeText(editor, text) {
    ControlFocus(editor)
    Loop Parse text
        SendMessage(0x102, Ord(A_LoopField), 0, editor)
}

; Reject any modal dialog owned by our process during the silent-save interval.
CheckNoDialog(name) {
    global testPid
    if WinExist('ahk_class #32770 ahk_pid ' testPid)
        Check(false, name)
}

; Find a named direct child of parent (zero defaults to root); zero means absent.
TreeFind(tree, name, parent := 0) {
    root := parent ? parent : SendMessage(0x110A, 0, 0, tree)
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
; expandButton targets the hit-tested directory toggle instead of its label.
ClickTree(tree, item, expandButton := false) {
    global testPid
    Check(item != 0, 'target tree item exists')
    SendMessage(0x1114, 0, item, tree)
    process := DllCall('OpenProcess', 'uint', 0x438, 'int', false, 'uint', testPid, 'ptr')
    memory := DllCall('VirtualAllocEx', 'ptr', process, 'ptr', 0, 'uptr', 64, 'uint', 0x3000, 'uint', 4, 'ptr')
    try {
        if !process || !memory
            throw Error('Cannot allocate remote tree hit-test buffer')
        data := Buffer(64, 0)
        NumPut('ptr', item, data)
        DllCall('WriteProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
        Check(SendMessage(0x1104, 1, memory, tree), 'target tree rectangle available')
        DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
        x := NumGet(data, 0, 'int') + 4
        y := (NumGet(data, 4, 'int') + NumGet(data, 12, 'int')) // 2
        if expandButton {
            found := false
            Loop x + 1 {
                x -= 1
                NumPut('int', x, 'int', y, data)
                DllCall('WriteProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
                SendMessage(0x1111, 0, memory, tree)
                DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 64, 'ptr', 0)
                if (NumGet(data, 8, 'uint') & 0x10) && NumGet(data, 16, 'ptr') = item {
                    found := true
                    break
                }
            }
            Check(found, 'target expansion button hit tested')
        }
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

; Read a real remote TVITEMW. Return empty for a node deleted during refresh;
; polling callers retry against the new root. Common controls need remote buffers.
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
            return ''
        DllCall('ReadProcessMemory', 'ptr', process, 'ptr', memory, 'ptr', data, 'uptr', 4096, 'ptr', 0)
        return StrGet(data.Ptr + 128, 'UTF-16')
    } finally {
        if memory
            DllCall('VirtualFreeEx', 'ptr', process, 'ptr', memory, 'uptr', 0, 'uint', 0x8000)
        if process
            DllCall('CloseHandle', 'ptr', process)
    }
}

; Read key from the UTF-8 Settings section of path; Win32 IniRead assumes ANSI or UTF-16.
; Return its text or the caller's optional defaultValue when the key is omitted.
; Missing sections and keys without an explicit default remain assertion failures.
ReadSetting(path, key, defaultValue?) {
    text := FileRead(path, 'UTF-8')
    if !RegExMatch(text, 'ms)^\[Settings\]\R(.*?)(?:^\[|\z)', &section)
        throw Error('Missing Settings section')
    if !RegExMatch(section[1], 'm)^' key '=([^\r\n]*)', &value) {
        if IsSet(defaultValue)
            return defaultValue
        throw Error('Missing Settings key: ' key)
    }
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
