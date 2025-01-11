# Tiny Xfer File (TXF)

---
## Description

A simple file transfer program for serial connection.


## Usage

```
A:\>txf
usage:  A:\TXF.EXE -s [speed] -l [com1-4]
        A:\TXF.EXR -s [speed] -l [com1-4] -f [filename]
A:\>
```


### Example

#### Receiver

Run receiver first, waiting for connection from sender.

```
A:\>txf -s 9600 -l com1
```

#### Sender

Next, send file to receiver.

```
A:\>txf -s 9600 -l com1 -f readme.md
```

## Limitation

- no support large file transfer, up to 0x7fffffff bytes
- no support long file name, up to 20 ASCII characters
- no support timestamp

No plan to fix them.

## License

WTFPL (http://www.wtfpl.net/)
