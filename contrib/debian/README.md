HowTo: Finit on Debian GNU/Linux
================================

HowTo use Finit to boot a Debian GNU/Linux system.  It is assumed that
the user has already installed a compiler, C library header files, and
other tools needed to build a GNU configure based project.  I.e., at
the very least:

    root@debian:~# apt install build-essential

Like the [Alpine HowTo](../alpine/), you need to install [libuEv][] and
[libite][], but since this is Debian — which takes infinite care of its
users ♥ ♥ ♥ we don't need to worry about `pkg-config`, except for having
it installed so it can locate the uEv and lite libraries.

> With Debian everything just works!™

... just make sure to install the following, so `ifup` and other basic
tools pre-systemd work as intended.

    root@debian:~# apt install initscripts console-setup

The following script can then be used to configure, build, install and
set up your system to run Finit:

    user@debian:~/finit$ contrib/debian/build.sh

However, since `/sbin/init` already exists on your system, the script
creates another entry in your GRUB config, in `/etc/grub.d/40_custom`,
where `init=/sbin/finit` is added to your kernel line.  It is of course
also possible to change the default init to Finit, if you do you can
remove the custom Grub entry:

    user@debian:~/finit# cd /sbin
    user@debian:/sbin# sudo rm init
    user@debian:/sbin# sudo ln -s finit init

Before rebooting, check the default [/etc/finit.conf](finit.conf) and
`/etc/finit.d/*.conf` files.  The build + install script above provides
a few sample `.conf` files. See `initctl list` after boot for a list of
enabled and available services.

You can also use a standard [/etc/rc.local](rc.local) for one-shot tasks
and initialization like keyboard language etc.

Have fun!  
 /Joachim ツ

[libuEv]: https://github.com/troglobit/libuev
[libite]: https://github.com/troglobit/libite
