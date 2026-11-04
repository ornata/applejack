# applejack controller

Somehow, many years ago, I came into possession of a controller for the [Apple Pippin](https://en.wikipedia.org/wiki/Apple_Pippin).

I wanted to try writing a device driver, and with AI basically eating the world of software, I sort of wanted to just write some C for fun.

I noticed that there wasn't much out there on this hardware, so I decided to give it a go.

This project requires that you have the following:

- A [Griffin iMate](https://apple.fandom.com/wiki/Griffin_iMate)
- A wired [AppleJack controller](https://pippin.fandom.com/wiki/AppleJack)
- A P-ADB <-> ADB dongle
- A macbook
- `libusb`

After you acquire that combination of things, you can play around with it like so:

```
$ make debug
$ sudo env OS_ACTIVITY_DT_MODE=YES ./build/debug/hidprobe
```

Will set up the controller so that you can use the gamepad to play a game.

