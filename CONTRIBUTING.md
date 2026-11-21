# Contributing

## AI disclaimer and usage policy

<!-- no, AI didn't write this part, the em dashes are here for comedic effect. -->
I — like many others — *don't* like AI that much. However, we can't deny that it *can* be useful. At times.
Now, writing such a massive project using only AI would be the dumbest thing i've heard all year, and that says something.
What i use it for is to help with documentation, commit messages (i have a script that takes the git diff and gives it to a local gemma4 to give me some commit messages i could use), and writing the tests (which is stupid and i fully acknowledge).
Also, as I'm not as experienced with C as I am with other languages, I often use a local model to translate some code from Haskell or Rust, which I then review.

> But then why write the project in C in the first place?

I feel like that's what most BSD people want as it's **THE** UNIX programming language, it interfaces better with the system, it's properly bootstrappable and it's what people are used to, in this space.

If the comments look sloppy or verbose at times, it's because of that. **If the code works, it's *precisely* because I didn't rely on AI for it.** If the code doesn't work, it's because i can write awful code all on my own !!! :> (look ma, no claude)

You are free to use whatever tools you want in your contributions, i mostly don't care *as long as* the code being written is maintainable and you can understand it, [**and it's a tool and not a replacement for your own reasoning.**](https://arxiv.org/abs/2502.12447)

My goal is to end up with no AI-generated code (so fully replacing the current tests with more robust ones, I think using AI to transpile between languages isn't exactly "AI-generated"), but i'm just a dude working on this project on my free time, and it took me long enough to get the code to a place i felt comfortable enough sharing, so excuse my occasional lazyness. ![](https://img.shields.io/badge/please_be_patient-i_have_autism-blue)

If you don't like this policy, feel free to open an issue and let the people decide, we live in a democracy.

## Sending a change

Run `sh tests/run-suite.sh` before you send anything; it prints what passed,
what failed and what was skipped, and a skip is not a pass. The build is
FreeBSD-only and `-Werror` is on, so a warning is a build failure.

