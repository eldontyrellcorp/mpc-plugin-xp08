Put your SR-JV80 expansion board dumps here (one 8 MB file per board, any file name).

XP-08 lists every file in this folder that reads as an SR-JV80 board; the slots A-D on the
SLOTS page choose which four are loaded. Dumps straight from a board reader (scrambled) and
already-descrambled images both work.

Optional: the JV-1080's own waves. Many board patches also use the instrument's internal
waves (INT-A, INT-B). Put the dumps of a JV-1080's chips here too, and those tones play:
  - the program ROM (1 MB),
  - the three INT-A wave ROMs (2 MB each, named so that they sort in chip order, e.g.
    jv1080_waverom1.bin, jv1080_waverom2.bin, jv1080_waverom3.bin),
  - the INT-B wave ROM (2 MB, e.g. jv1080_waverom4.bin).
XP-08 recognises them by their contents and does not list them as boards.

Optional too: the JV-80's own waves, for the boards made for the JV-80 (Pop, Orchestral, Piano,
Vintage Synth, World, Experience), whose patches use them. A JV-880 carries the same waves: put
the dumps of its program ROM ("rom2", 256 KB; with two versions, the later file name is used) and
of its two wave ROMs (2 MB each, named so that they sort in chip order, e.g. jv880_waverom1.bin,
jv880_waverom2.bin).

The dumps are Roland's data: they are not part of the plugin, and you should only use dumps of
boards and instruments you own. Do not share a plugin folder that has them in it.
