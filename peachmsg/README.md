**peachmsg** - send ipc messages to peachwm (forked from [this repository](https://codeberg.org/notchoc/dwlmsg))

```
usage:	peachmsg [-OTLPF]
	peachmsg [-o <output>] -s [-t <tags>] [-l <layout>] [-c <tags>]
	peachmsg [-o <output>] (-g | -w) [-FOotlcvmf]
```

```
options:
-g	get
-s	set
-w	watch
-O	get all outputs
-T	get number of tags
-L	get all available layouts
-P	get compositor pid
-F	get focused output
-o	select output
-t	get/set selected tags (set with [+-^.], overwrite with ! prefix)
-l	get/set current layout
-c	get title and appid of focused client
-v	get visibility of statusbar
-m	get fullscreen status
-f	get floating status
```

```
examples:
	# act like dwl stdout
	peachmsg -w
	# watch focused client appid and title
	peachmsg -w -c
	# get currently focused output
	peachmsg -F
	# watch for focused output changes
	peachmsg -w -F
	# get all available outputs
	peachmsg -O
	# watch available outputs
	peachmsg -w -O
	# select tag 1, deselect tag 2, toggle tag 4 on output eDP-1
	peachmsg -o eDP-1 -s -t +-.^
	# toggle tag 3, overwriting current tagset (yes, zero-indexed)
	peachmsg -t !2^
	# select tag 8 on current output
	peachmsg -s -t 7
	# deselect tag 8 on current output
	peachmsg -s -t 7-
	# switch to first layout (order given by peachmsg -L)
	peachmsg -l 0
	# switch to floating layout
	peachmsg -l '><>'
```

