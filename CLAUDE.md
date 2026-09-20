# Instructions for Claude

Read `coding-rules-for-ai-chatbots.txt` in this directory before writing any code. It is the
authoritative style guide for this project and it is long, specific and enforced in places by
`scripts/check_standards.py`. Everything below is in addition to it.

## Write in plain English. Length is not a cost.

This is the rule that gets broken most often, so it comes first.

Andrew's words, given more than once:

> "I continually have to tell you to speak in plain english. I do not mind that it makes things
> longer. I dont care about your efforts to save on your typing, that is a ludicrous concern,
> stop it at once."

> "while you are writing all these comments please make sure that clarity is the aim, not
> getting the number of characters typed to a minimum."

> "you have a tendency to harp on about previous designs and to employ your own weird
> metaphors, abbreviations, made up terminolgy and cryptic phrasing."

This applies to **everything written**: code comments, Doxygen, commit messages, Markdown,
TOML and YAML comments, and prose written to Andrew in conversation.

The specific failure to watch for is **labelling a thing instead of explaining it**. A phrase
like "the family every component on the order path contributes a checkpoint to" names three
concepts and explains none of them. It is only readable by someone who already knows the
answer, which is never the person who needs the comment. Write the sentences that say what
actually happens, and write a second and a third if one is not enough.

The rules file already bans shortened identifiers "to save on typing". The same reasoning
applies to prose, and with more force, because prose has no compiler to make the meaning
recoverable.

## Do not write about the past

A document or a comment says what the system does now. It does not say what the design used to
be, what was tried before, what an earlier session decided, or what was corrected. A reader
months from now was not present for any of it.

> "when you produce documentation of any kind, latex, markdown, doxygen, code comments, or
> anything, in general you should not hark on about the past or refer to previous conversations
> we have had, neither should you invent terminology of your own or weird non-standard
> abbreviations to save on typing."

The rules file states two cases of this for code: no comments referring to previous
conversations, and none referring to previous AI chatbot corrections. The rule is the same one,
widened to everything written.

An exception, because it is the opposite of harking back: a measurement and the conclusion
drawn from it are current facts, not history. `docs/operations/latency_findings.md` records
what has been measured and what has been ruled out, so that a change already tried and found
not to work is not proposed again. Write those in the present tense and state the evidence.

## Do not invent terminology or abbreviations

Use the words the field already uses, or the words this project already uses. A term coined to
name a relationship reads as settled vocabulary the reader is expected to look up, and there is
nowhere to look it up. Expand abbreviations: write "execution report", not "ER"; "matching
engine", not "ME". Never invent a new short form to save typing.

Do not use the phrase "hand waving", and do not substitute a near-synonym for it either. Where
the point is that something was verified rather than assumed, say what was actually done.

## Verify, do not assume

Several wrong conclusions in this project came from instruments that were not measuring what
they appeared to measure. Before reporting that a check passes, make it fail on purpose. Before
reading a number, confirm the thing producing it is the thing intended.

When killing a process, check it actually died rather than trusting the command's exit status.

## Working practice

- Commit straight to `main`. Do not create a branch unless asked, and do not offer one.
- Never `git add -A` in this repository; add named paths.
- This is a public repository. No employment context, customer names, or internal system names
  from Andrew's work belong in it.
- The developer loop is `scripts/devsetup.sh`, which builds, releases, deploys and tests. Do not
  substitute `build.sh` followed by `deploy.py`; that skips the release step.
- Maximum line length is 160 for source and configuration. Markdown and prose are exempt.
