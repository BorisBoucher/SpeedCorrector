# SpeedCorrector

The speed corrector is a little electronic devices driven by an Arduino Nano that
allow correction of the displayed speed on speed gauge.

I made this devices after realizing that my 3000GT speed gaude was totaly off : when driving at 50km/h, the gauge was near 60!
Same defect at high speed, a little less however.


After taking some samples at different stabilized speeds with GPS speeds and gauge speeds, I have a set a correction factor
for build a 3 points correction curve. 