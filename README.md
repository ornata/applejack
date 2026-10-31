# pippin controller

Somehow, many years ago, I came into possession of a controller for the Apple Bandai Pippin along with an iMate USB adapter.

I think this is an interesting piece of hardware, and I wanted to write some code. I'd never written any driver code before, so I wanted to give it a try.

```
$ make debug
```

Builds a simple probe, which can look at which buttons were pressed on the device if it is connected to a computer.

I'm doing this on a macbook, so I'm assuming that you have CoreFoundation + IOKit.
