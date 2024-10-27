### How to verify the release archive?

You can download the release archive from the
[libevent.org](https://libevent.org) or from [github
releases](https://github.com/libevent/libevent/releases), but note, for later
you need `*.tar.gz*` files (sicne `Source code` is the archive that is
generated by github and they do not includes pre-configured scripts for
`autoconf`)

```
$ wget https://raw.githubusercontent.com/azat/azat.github.com/master/azatpub.asc
$ gpg --import azatpub.asc
# Or
$ gpg --recv-key 9E3AC83A27974B84D1B3401DB86086848EF8686D

$ wget https://github.com/libevent/libevent/releases/download/release-2.2.1-alpha/libevent-2.2.1-alpha-dev.tar.gz
$ wget https://github.com/libevent/libevent/releases/download/release-2.2.1-alpha/libevent-2.2.1-alpha-dev.tar.gz.asc
$ gpg --verify libevent-2.2.1-alpha-dev.tar.gz.asc
```

*You can find my public key [on github](https://raw.githubusercontent.com/azat/azat.github.com/master/azatpub.asc)*

You may also want to check the key signatures:

```
$ gpg --list-signatures 9E3AC83A27974B84D1B3401DB86086848EF8686D
# Download missing keys and then
$ gpg --check-signatures 9E3AC83A27974B84D1B3401DB86086848EF8686D
```