# Bomberman World — Remote Blast Arena

**Student:** Derin Türk

## How to play

- **Controls:** Move with WASD or the arrow keys. Press Space to place a bomb, E to remotely detonate every live bomb you own, R to restart after a win or loss, and Q or Escape to quit.
- **Objective and rules:** Defeat both enemies without touching an enemy or a blast. A hit loses the round; a round is won when the second enemy disappears. If a blast kills the final enemy and the player in the same simulation frame, Lost takes priority. Bombs are solid after the owner has stepped away, but the owner can escape the bomb they just placed. The green pickup increases the range of later bombs from 3 to 4.
- **Build configuration / demo setup:** Built in Visual Studio 2022 with the v143 platform toolset and Warning Level 4. The fixed arena contains two randomly wandering enemies, twelve breakable crates, and one fixed pickup so every feature can be demonstrated quickly.

## Features and validation

| Feature                          | Check, observed result, and expected behaviour                                                                                                                                                                                                                          |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Player damage and death       | Move into an enemy or remain in a blast tile. The state changes to LOST once and movement/bomb input stops until R restarts the round. Walking near, but not into, an enemy does not end the round.                                                                     |
| 2. Round states and objective    | Defeat both red enemies to show WON. Lose a separate round by touching an enemy; both result states are displayed and freeze gameplay.                                                                                                                                  |
| 3. Safe restart                  | Press R after a win/loss, including after placing bombs. The original map, player, pickup, spatial grid, effects, timers, and update lists are rebuilt; old bombs do not affect the new round.                                                                          |
| 4. Solid bombs with owner escape | Place a bomb, walk away, and try to re-enter its tile: re-entry is blocked. The owner can leave the newly placed bomb, other actors do not get that exception, and the tile becomes passable after detonation. A second bomb cannot be placed on an occupied bomb tile. |
| 5. Blast-range pickup            | Place a bomb before taking the green pickup, then collect it and place another. The range display rises from 3 to 4, and only the later bomb uses the longer captured range.                                                                                            |
| 6. Multiple bombs per owner      | Place two bombs on separate tiles, then try a third: it is rejected. Detonate one and another placement becomes available; repeating the cycle keeps capacity correct.                                                                                                  |
| 7. Chain reactions               | Put two bombs in a line with different remaining fuse times. Detonating the first requests the second detonation through the same fact pipeline; the second explodes immediately and never explodes again later.                                                        |
| 13. Remote-detonated bombs       | Place one or two bombs and press E. Only valid live bombs owned by the player detonate; pressing E with no live bombs is harmless, and normal fuses remain available as a fallback.                                                                                     |

**Known issues:** None known after manual testing. The compact fixed arena is intentionally a demonstration map rather than a procedural level.

## Sources and assistance

This project builds on the shared Kotte foundations (the `2026-week-06-reference` tag) and the supplied Bomberman assets. I used AI assistance and Reddit discussions to help me understand concepts when I got stuck. They were used for learning and troubleshooting. I reviewed, adapted, and tested all code myself, and the final implementation was completed by me.
