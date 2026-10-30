# PhD pathway with Nanoer

A plan for an undergraduate engineering student to use Nanoer as the
research project behind a PhD application. Policies, deadlines and
programme rules change; check each one at the source before relying on it.

## First, the misconception

**A good paper does not hand you a PhD.** A PhD is a 4–6 year research
programme you are *admitted* to, not a degree awarded for one publication.
Admissions committees decide using, roughly in this order:

1. **Recommendation letters**, above all from researchers who supervised you.
2. **Research experience**: what you investigated, what you found, and
   whether you can explain it.
3. **Statement of purpose**: why you want to do research, and with whom.
4. **Grades**, especially in core systems courses.
5. **Publications**: a strong plus, but not required, and never enough on
   their own.

A paper helps because it is evidence of the second point, and because the
professor who helped you write it can write the first.

## Where Nanoer stands

**Strengths**

- A working read-write FUSE filesystem with a C++20 engine (M1, M2).
- Serious testing: unit tests, real-mount integration tests, fuzzing,
  sanitizers.
- Real measurements (`KNOWLEDGE.md` §14): 71 % less disk than ext4 on
  OS-like content; codec trade-offs between LZ4, zstd and LZMA.
- Documented design: `docs/format.md` and the decision log.

**Gaps before it counts as research**

- **No new idea implemented yet.** Compressed filesystems are well studied
  (Btrfs, ZFS, EROFS, squashfs, DwarFS, dm-vdo). The candidates for a
  contribution are hot/cold tiering, dedup plus compression within the safe
  space, and per-folder policies, none of which is built.
- **Wrong baseline.** Benchmarks compare against ext4, which does not
  compress. The yardstick is Btrfs with zstd.
- **Weak statistics.** Single runs on one VM; no error bars, no standard
  benchmarks.

## Step 1: Strengthen the basics (start now, ongoing)

- **Grades**: aim high in operating systems, computer architecture, data
  structures and algorithms, and networks.
- **Theory**: read *Operating Systems: Three Easy Pieces* (free online; the
  file system chapters), then the classic papers on LFS, ZFS, Btrfs, and
  dedup and compression papers from USENIX FAST.
- **Own the code.** Nanoer was written with AI assistance, so interviewers
  will probe your understanding. Be able to explain the on-disk format,
  journaling, crash safety and codec trade-offs without notes.
  `KNOWLEDGE.md` is the study guide.

## Step 2: Turn the project into research (next 6–12 months)

1. **Finish M3**: crash safety and fsck.
2. **Build one new idea**, most likely hot/cold tiering. §14 already hints
   at the payoff: moving cold data from zstd 1 to zstd 19 saves about 5
   more points of the original size and still decompresses at ~780 MB/s.
3. **State a hypothesis**, for example: "automatic recompression of cold
   data saves X % space compared with Btrfs+zstd, at a cost of Y %
   throughput."
4. **Evaluate properly**:
   - compare against Btrfs+zstd (and possibly ZFS);
   - repeat runs and report variance;
   - use standard benchmarks (fio, filebench) and several real datasets.

## Step 3: Find a mentor (start now)

This step does the most for your chances.

- **At your college**: find a professor in systems, storage or operating
  systems. Show them Nanoer and ask them to supervise it as a research or
  final-year project.
- **Outside your college**: email professors whose recent FAST or
  HotStorage papers cover compression, dedup or tiering. Keep it short: one
  line about you, two lines on Nanoer and your results, the GitHub link,
  and one specific question about their paper.
- **Research internships**:
  - US: REU (Research Experiences for Undergraduates) and university summer
    programmes;
  - Europe: summer programmes at places such as EPFL, ETH Zürich and the
    Max Planck institutes;
  - India: summer fellowships at the IITs and IISc, and the Indian
    Academies' summer research fellowships;
  - industry research labs such as Microsoft Research and Google.

## Step 4: Publish (final 1–1.5 years of the degree)

| Order | Venue | Why |
|---|---|---|
| 1 | arXiv preprint | Records the work publicly; get your mentor's feedback first |
| 2 | USENIX HotStorage (workshop) | Short papers on new ideas; realistic for an undergraduate; well regarded |
| 3 | USENIX ATC or FAST (full paper) | Top venues for storage systems; aim here once results are strong |
| Fallback | ACM Transactions on Storage, ACM SYSTOR, *Software: Practice and Experience* | Respected options if the top conferences reject the paper |

How these look on a PhD application:

| Venue | Signal |
|---|---|
| FAST, ATC, EuroSys | Very strong; few applicants have one |
| ACM TOS | Strong; systems researchers weigh top conferences above journals |
| HotStorage, SYSTOR | Good; shows real peer-reviewed research |
| IEEE Access and similar journals | Helpful, but weigh less with top systems groups |
| arXiv only | Not peer-reviewed, but with the open-source code it shows serious work |

**AI disclosure.** IEEE, ACM, USENIX and the major publishers do not reject
AI-assisted work, but an AI cannot be an author, its use must be disclosed
(usually in the acknowledgments), and you are responsible for everything in
the paper. Read the current policy of each venue before submitting.

**Avoid predatory journals**: unsolicited invitations, high fees, very fast
acceptance. Check that a venue is indexed in DBLP, Scopus or Web of Science.
A predatory publication can hurt an application.

## Step 5: Apply (final year)

| | US | Europe | India |
|---|---|---|---|
| Entry after a bachelor's | Directly to PhD (5–6 years) | Usually needs a master's first (some, such as ETH and EPFL, take direct entry) | Direct PhD possible at the IITs and IISc; GATE helps |
| Funding | Usually fully funded with a stipend | Usually a paid researcher position | PMRF (Prime Minister's Research Fellowship) for top candidates |
| Deadlines | Mostly December | Varies, often rolling | Varies by institute |

**The application**

- **Three recommendation letters**, at least one from a research mentor.
- **Statement of purpose**: tell the Nanoer story as research — the
  problem, the questions, your design decisions, what you measured and
  learned — then name the professors you want to work with and why.
- **CV**: papers, the GitHub repository, internships, grades.
- **Tests**: the GRE is optional at most US universities; TOEFL or IELTS if
  English is not your first language.

**Choosing where to apply**: use CSRankings (systems areas) and the author
lists of recent FAST and HotStorage papers. The professors who write papers
on compression, dedup or tiering are the people to email and the advisors
to target.

## Example timeline (third-year student)

| When | Goal |
|---|---|
| Next 3 months | Finish M3; read the classic papers; contact professors |
| Summer | Research internship, or build tiering with a mentor |
| Final year, semester 1 | Btrfs evaluation; arXiv preprint; workshop submission; PhD applications (December for the US) |
| Final year, semester 2 | Interviews and decisions; submit the full paper |

A final-year student should run Steps 2–4 in parallel and expect to apply a
year later than planned, for example after a master's or a research
assistant position.

## Common mistakes

- Applying without anyone who can write a research letter.
- A statement of purpose that lists features ("I built a filesystem")
  instead of the questions asked and answered.
- Being unable to explain your own code in an interview.
- Publishing in a predatory journal.
- Treating the paper as the goal; it is evidence for the application, not
  the admission ticket.
