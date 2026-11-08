# flatpkg demo: one package transaction, end to end.
#
# Regenerate with:   vhs .gh/demo      (VHS reads .gh/demo.tape)
# Output:            .gh/demo.gif

Output .gh/demo.gif
Require flatpkg
Require ttyd
Require ffmpeg

Set Shell "bash"
Set FontSize 18
Set Width 1280
Set Height 720
Set Theme "GitHub Dark"
Set BorderRadius 8
Set TypingSpeed 69ms
Set Framerate 24
Set PlaybackSpeed 1
Set WaitTimeout 1m

Hide
Type "rm -f /tmp/hello.md; export TERM=xterm-ghostty COLORTERM=truecolor NO_COLOR= FORCE_COLOR=1; bash -l"
Enter
Type "cd /; clear && uname -a"
Enter
Sleep 6s
Show

# install: resolve the name, confirm the ref, show the changeset, pull and check out
Type "flatpkg install helix" Enter
Wait@20s /Use this ref?/
Sleep 1s
Type "y"
Wait /changes to the system/
Sleep 1s
Type "y"
Wait+Screen /transaction/
Sleep 1s

Type "flatpkg run com.helix_editor.Helix /tmp/hello.md" Enter
Wait+Screen /NOR/
Sleep 4s
Type ":theme github_dark" Enter
Sleep 1s
Type "i"
Type "# flatpkg demo" Enter
Type "edited from inside a FreeBSD jail" Enter
Type "- Helix running on "
Escape
Type ":insert-outputuname -a"
Left 8
Space Enter
Type "A"
Enter
Type "- Installed as an unmodified Linux application" Enter
Sleep 4s
Escape
Type@360ms ":wq" Enter
Sleep 3s

# show what the store and the jail did
Type "cat /tmp/hello.md"
Enter
Sleep 2s
Type "flatpkg info com.helix_editor.Helix"
Enter
Sleep 2s
Type "flatpkg ps"
Enter
Sleep 2s

Type "flatpkg uninstall -y com.helix_editor.Helix"
Enter
Sleep 10s
