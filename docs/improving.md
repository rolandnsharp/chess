# Improving at chess efficiently: what the evidence says, and how this trainer uses it

Written November 2026 from a literature search (OpenAlex/Semantic sources via
ScienceMadeReadable) and the main practitioner sources. Each claim is marked with how
strong the evidence is. The personal application for this trainer's player lives in
`data/coaching.md` (not in the repo).

## The short version

1. **How you train matters more than how much you play.** Study activities improve
   rating several times faster per hour than playing games. *(moderate: large
   observational studies)*
2. **Slow down.** Faster games produce more blunders, and spending more time thinking
   predicts improvement. *(moderate)*
3. **Review your own games.** It is the best-measured study activity for amateurs.
   *(moderate, observational)*
4. **Below ~1700, games are lost to unsafe moves.** That means pieces left hanging and
   threats not seen; strategy is rarely the problem. Fix this with a checking habit
   (checks, captures, threats on every move), not with more theory.
   *(practitioner consensus; consistent with blunder statistics)*
5. **Puzzles help most when chosen for accuracy, not speed.** Use easy-to-medium ones,
   repeat them on a spaced schedule, and include defensive puzzles and red herrings.
   *(weak direct evidence; strong support from learning science)*
6. **Openings matter little at this level.** *(practitioner consensus, weak data)*

There is **no controlled trial** of any chess training method on adult ratings.
Everything below is either observational data or coaching experience, with learning
science filling the gaps. Treat each method as an experiment on your own results
(see "Measuring" at the end).

## Findings

### 1. Deliberate study beats playing, per hour. Evidence: moderate

- **Southwick et al. 2026, *Psychological Science*.**
  - 44,213 chess.com players followed for six months, using time-stamped records of
    what they actually did. <https://doi.org/10.1177/09567976261452568>
  - Players spent over 90% of their time playing games.
  - Yet deliberate-practice activities as a group were **3.61× more efficient per hour**
    than games.
  - A secondary summary gives per-activity figures; I could not check them against the
    paper. For players rated 400–999 it lists about 0.9 rating points per hour from
    games, about 4.9 from game review and about 5.2 from lessons. Puzzles were no
    better than games.
- **Older studies agree.**
  - Charness et al. 2005: serious study alone was the strongest predictor of rating.
    <https://doi.org/10.1002/acp.1106>
  - Burgoyne et al. 2019: serious chess activity, instruction and starting age
    explained 63% of the variance in peak rating.
    <https://doi.org/10.5406/amerjpsyc.132.1.0027>
- **Disagreement.**
  - Howard 2011 found the number of games played mattered and study hours barely did.
    <https://doi.org/10.1002/acp.1834>
  - Macnamara et al. 2014 (meta-analysis): practice explains about 26% of skill in games.
    <https://doi.org/10.1177/0956797614535810>
  - People differ a lot: one player needed 8× another's practice to reach master
    (Gobet & Campitelli 2007). <https://doi.org/10.1037/0012-1649.43.1.159>
  - So expect average gains, not guaranteed ones.

### 2. Reviewing your own games is associated with improvement. Evidence: moderate

- Yiannakoulias 2026 studied about 2 million online speed games.
  <https://doi.org/10.1177/10468781261443352>
  - Analysing games predicted rating gains.
  - Analysing *wins* predicted larger gains than analysing losses.
  - The number of games played showed only a weak link to improvement.

### 3. Time to think. Evidence: moderate

- The same grandmasters blunder more often and more severely in rapid than in classical
  games (Chabris & Hearst 2003). <https://doi.org/10.1207/s15516709cog2704_3>
- In 1,873 lichess players followed over time, **more time spent thinking before moves**
  predicted later improvement (Schut et al. 2025).
  <https://escholarship.org/uc/item/5c76v07h>
- Dan Heisman's practical rule: a full safety check on every move needs about
  **30 minutes or more per side**.

### 4. Skill is pattern recognition, so train patterns that actually occur in games. Evidence: strong theory, weak training evidence

- **Chunk and template theory** (Chase & Simon 1973; Gobet & Simon 1996): strong players
  recognise familiar patterns of pieces.
- **Transfer is narrow** (Sala & Gobet 2017): practice improves the specific thing
  practised, so drill patterns you will actually meet.
  <https://doi.org/10.1177/0963721417712760>
- **The Einstellung effect:** a familiar idea can stop you seeing a better one
  (Bilalić et al. 2007). Include "red herring" positions where the obvious shot fails.
  <https://doi.org/10.1016/j.cogpsych.2007.02.001>

### 5. Puzzles: popular, weakly evidenced. Evidence: weak

- **No study shows that puzzle training raises adult ratings.** Puzzle and game ratings
  correlate, but that doesn't mean one causes the other.
- In the Southwick data, puzzle time was no more efficient than playing. Speed modes
  that reward re-solving known patterns may explain that; this is unproven.
- **Practitioners agree on drilling *basic* patterns until they're automatic,** and
  solving hard ones only once.
  - Heisman.
  - The Woodpecker Method (Smith & Tikkanen 2018): solve a fixed set, rest, then
    re-solve faster.
  - de la Maza's *Rapid Chess Improvement*: one person's anecdote, and its claims are
    contested.
- **Difficulty:** the only principled figure found is about **85% success** as the best
  level for learning (Wilson et al. 2019, from machine-learning models; weak support for
  humans). <https://doi.org/10.1038/s41467-019-12552-4>

### 6. Learning science: retrieve, space, mix, adapt. Evidence: strong in general, untested in chess

- Testing yourself beats re-reading (Roediger & Karpicke 2006).
  <https://doi.org/10.1111/j.1467-9280.2006.01693.x>
- Spaced practice beats massed practice (Cepeda et al. 2006).
  <https://doi.org/10.1037/0033-2909.132.3.354>
- Mixing problem types beats practising one type at a time (Rohrer et al. 2014).
  <https://doi.org/10.1037/edu0000001>
- Schedules that adapt to each item's accuracy and speed beat fixed schedules
  (Mettler et al. 2016). <https://doi.org/10.1037/xge0000170>

### 7. Openings at low ratings. Evidence: weak

- Practitioners agree: learn principles plus one simple system per colour, and
  study tactics before openings.
- Data: experts narrow their repertoire, while beginners exploring *more varied*
  openings predicted improvement (Schut 2025).

## How this trainer applies it

| Finding | In the trainer |
|---|---|
| Unsafe moves lose games below ~1700 | `/threats` (name what the opponent's last move threatens, or `none`) and the opponent half of `/scan` (list all their checks and captures) drill the safety check itself, using positions from your own games |
| Train patterns that occur in *your* games (narrow transfer) | `/drill puzzles` and `/drill punish` are your own blunders and your opponents' unpunished ones |
| Retrieval + spacing + adaptivity | Every drill is spaced (1, 3, 7, 21, 60 days); misses come back the same session or the next day |
| ~85% success; accuracy over speed | Lichess puzzles are picked about **300 points below your puzzle rating**, where the Elo formula expects you to score 85%. They are untimed and only the first try counts toward your rating |
| Defensive puzzles and red herrings | Defensive-move and quiet-move puzzles can be targeted (`/puzzles defence,quiet`). About one threat-spotting position in six has no threat |
| Mixing problem types | A `/train` session moves between drill types; lichess puzzles mix themes unless filtered |
| Openings matter little | Opening work is limited to your own lines, and only where your usual move costs you |
| Game review is the most efficient study | `/review`: mark your own mistakes first (with the better move and why), then `/engine` scores you against Stockfish; wins included (`/review win`) |
| More thinking time | A training rule rather than a feature: play rated games at 15+10 or slower |

## Not built yet, ranked by expected value

1. **A pre-move checklist and clock coaching in practice games.** Warn when a move was
   played in under 2 seconds in a non-forced position.
2. **A Woodpecker set:** a fixed set of 200–400 basic-motif puzzles, solved in cycles,
   aiming to halve the total time each cycle.

## Measuring

Rating is noisy. The trainer's main measure is **engine-measured blunders per 100
moves at a fixed time control**, tracked weekly by `make update`, plus the practice
logs (scan hit rate and time, threat-spotting rate, puzzle rating). Every change to
the plan is judged against your own previous numbers.
