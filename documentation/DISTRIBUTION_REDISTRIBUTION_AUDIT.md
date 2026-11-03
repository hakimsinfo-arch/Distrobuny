# Distro Buny — Could a future edition ship with distribution ISOs?

Status: research notes for the owner, 24 October 2026. **Not legal advice.**
Every conclusion below needs review by a qualified lawyer before an edition
with bundled ISO files is published. The 1.0.1 release contains **no**
distribution ISO files, and none were bundled as part of this audit.

## 1. The question

Distro Buny was first imagined as one download that already contains many
Linux ISO files. The released starter image instead lets users copy their
own. This note asks whether a preloaded edition is feasible, distribution by
distribution, from three angles:

1. **Copyright licences of the software inside the ISOs.** Most of it is free
   software (GPL, LGPL, MIT, BSD, ...) and may be redistributed, but the GPL
   and LGPL attach conditions to redistribution (below).
2. **Trademarks.** Distribution names and logos are trademarks with their own
   policies, separate from the software licences.
3. **Components with special terms.** Firmware, codecs, proprietary drivers
   and applications that some ISOs include.

## 2. Findings that apply to every distribution

* **Redistributing an ISO makes the publisher of Distro Buny a distributor of
  every GPL program on it.** The GPL (v2 §3, v3 §6) requires a distributor to
  provide the complete corresponding source code, or a written offer valid
  for three years, for the exact versions shipped. Distributions meet this
  through their own source repositories. A third party that ships their ISOs
  generally has to make the same source available itself (GPLv3 allows
  pointing to a third-party server only if the distributor ensures it stays
  available). For 40+ ISOs this means keeping the complete source of each
  shipped version available for years: tens to hundreds of GB, and an
  ongoing obligation. **This alone is the largest practical obstacle.**
* **Redistributing unmodified images is generally what trademark policies
  allow most easily; modifying them is what they restrict.** Distro Buny never
  changes an ISO file (they are copied byte for byte), which keeps a bundled
  edition in the "unmodified" category. Branding the bundle as Distro Buny,
  and saying "includes Ubuntu, Fedora, ..." is descriptive use, which the
  policies below mostly allow, but only for unmodified copies and without
  implying endorsement.
* **The logos in the Distro Buny menu are a separate question** from ISO
  redistribution: they are used today, in the released starter image, to
  identify the user's own ISO files (nominative use, from the Simple Icons
  collection). The policies below restrict logo use more than name use. This
  should be reviewed even if no ISO is ever bundled (§6, question 2).
* **Selling** a bundle, as opposed to offering it free, triggers more
  restrictions in several policies (commercial use of marks, merchandise).

## 3. Per distribution

Classification:
**A** = potentially redistributable under identified conditions;
**B** = requires further permission or review;
**C** = not suitable for bundling without additional authorisation.

| Distribution | What the official policy says (source in §5) | Special components | Class |
| --- | --- | --- | --- |
| **Ubuntu and official flavours** (Kubuntu, Xubuntu, Lubuntu, Ubuntu MATE, Budgie, Studio, Kylin, Unity, Cinnamon, Edubuntu) | Canonical's IPRights Policy: "You can redistribute Ubuntu, but only where there has been no modification to it", "in its unmodified form, complete with the installer images and packages provided by Canonical". Modified versions "must be approved, certified or provided by Canonical", otherwise trademarks must be removed and the source recompiled. Trademark use otherwise needs "Canonical's permission in writing"; "You cannot use the Trademarks in software titles". Policy dated 15 July 2015. | some flavours carry restricted firmware/drivers in their pool | **A** for unmodified ISOs (plus the GPL source obligation of §2); flavours are covered by the same policy, but each flavour's own branding should be checked |
| **Fedora editions** (Workstation, KDE, Server, IoT, Atomic desktops) | Fedora's trademark guidelines (docs.fedoraproject.org/en-US/legal/trademarks/) define "Fedora Media" to include unmodified reproductions, including live USB media. The page could not be retrieved verbatim during this audit (bot protection); secondary sources and a Fedora wiki draft indicate unmodified media may be labelled with the marks under conditions (attribution, no implied endorsement), and that modified builds need a different name ("Remix"). | Fedora ships only free software and no patent-encumbered codecs | **A**, after the exact current guideline text is checked |
| **Debian** | Debian's trademark policy (debian.org/trademark) does not address image redistribution specifically. It allows describing products "relating to Debian" if not misleading, forbids the Debian marks "as the name of a product or service" and logos "as part of your company logo or product logo"; other uses go to trademark@debian.org. Debian's licence policy (DFSG) is designed for redistribution. | official images include **non-free firmware** since Debian 12; its licences permit redistribution but should be checked per package | **A** (and the netinst image cannot be started by Distro Buny 1.0.1 anyway) |
| **Arch Linux** | Arch's trademark policy (terms.archlinux.org): a "substantially unmodified Arch Linux product" may carry the name; otherwise it is a "Remix". Commercial use, merchandise and material changes need "Explicit permission from the Arch Linux Project Lead"; the ™ declaration must stay with the logos. | none of note | **A** for a free, unmodified bundle; **B** if sold |
| **Linux Mint** | No current official trademark or redistribution policy was found on linuxmint.com. An old Linux Mint blog post (c. 2008) records that the Linux Mark Institute approved the name. Mint is built on Ubuntu packages; in 2014 Canonical stated that binary redistributors such as Mint need a licence for Ubuntu's marks (Ubuntu Community Council statement, reported by LWN). | multimedia codecs are optional, not on the ISO | **B**: ask the Linux Mint team |
| **Bazzite** (Universal Blue) | The project's own code is Apache-2.0 (per its repository). A project representative wrote on the Universal Blue forum that the project cannot stop anyone shipping it, but that anyone charging for it should get a green light from **Valve** (Steam, the main proprietary component) and, for the NVIDIA image, from **NVIDIA** (proprietary driver), and that H.264 licensing varies. These are informal forum answers, not a policy. | **Steam** (proprietary), **NVIDIA driver** (proprietary, nvidia image), codecs | **C** for the NVIDIA image and any paid bundle; **B** otherwise |
| Pop!_OS | System76 trademarks; built on Ubuntu (Canonical's policy for Ubuntu packages may apply). The NVIDIA image ships the proprietary NVIDIA driver. | NVIDIA driver | **B** (NVIDIA image **C**) |
| openSUSE | openSUSE trademark guidelines (en.opensuse.org/openSUSE:Trademark_guidelines) were not reviewed in detail. | some codecs/firmware | **B** |
| Kali Linux, Parrot OS, BlackArch | Security distributions. Kali is a trademark of OffSec, which publishes a trademark policy; not reviewed in detail. | firmware; security tools whose own licences vary | **B** |
| Red Hat Enterprise Linux | RHEL images are obtained under a Red Hat subscription agreement and carry Red Hat trademarks. | Red Hat branding | **C**: use CentOS Stream or AlmaLinux instead |
| CentOS Stream, AlmaLinux | Freely downloadable rebuilds; each has its own trademark (Red Hat for CentOS, AlmaLinux OS Foundation). Not reviewed in detail. | none of note | **B** |
| Manjaro, EndeavourOS, CachyOS, NixOS, Guix, Void, Gentoo, Nobara, Zorin OS, MX Linux, antiX, Alpine, Slackware | not reviewed in this audit | varies | **B** |

## 4. Recommendation

1. **Keep the starter image as the public product.** It avoids every
   question above: users download each ISO from its publisher, who also
   provides its source code.
2. A preloaded edition, if ever wanted, is most realistic as a **small, free
   bundle of unmodified ISOs** from distributions in class A, with
   * the complete corresponding source of every bundled ISO made available
     for three years (or a lawyer-approved alternative),
   * each distribution's attribution statement and no suggestion of
     endorsement,
   * written confirmation from each project where class B applies.
3. Never bundle class C images (RHEL, NVIDIA-driver images, Steam-based
   images) without explicit written authorisation from the rights holders.

## 5. Sources consulted

Accessed on 24 October 2026. Quotes in §3 come from the first three; the
others were consulted through search results and are marked as such.

* Canonical, *Intellectual property rights policy*: https://canonical.com/legal/intellectual-property-policy (ubuntu.com/legal/intellectual-property-policy redirects there)
* Debian, *Debian Trademarks*: https://www.debian.org/trademark
* Arch Linux, *Trademark Policy*: https://terms.archlinux.org/docs/trademark-policy/
* Fedora, *Trademark guidelines*: https://docs.fedoraproject.org/en-US/legal/trademarks/ (not retrievable during the audit; secondary: https://fedoraproject.org/wiki/Distribution, https://fedoraproject.org/wiki/User:Pchestek/TMGuidelinesDraft)
* Linux Mint blog: https://blog.linuxmint.com/?p=159 (search result); LWN coverage of Canonical's licensing requirement: https://lwn.net/Articles/587094 (search result)
* Universal Blue forum: https://universal-blue.discourse.group/t/is-it-legal-to-sell-a-prebuilt-with-bazzite-installed/1799 and https://universal-blue.discourse.group/t/can-i-use-the-bazzite-for-business/2862 (search results)
* GNU General Public License v2 §3 and v3 §6: the licence texts in `Licenses and source code/licenses/` of the release

## 6. Open questions for a lawyer

1. Does shipping many unmodified ISOs in one image count as "mere
   aggregation" for each ISO, and what exactly must the publisher then
   provide as corresponding source, for how long?
2. Is the current menu's use of distribution logos (to label the user's own
   ISO files) nominative fair use under each project's policy and the
   relevant (EU, German) trademark law, or is permission needed?
3. Which name may a bundle carry ("Distro Buny with Ubuntu, Fedora, ...")?
4. What changes if a future company sells the bundle or preloaded sticks?
