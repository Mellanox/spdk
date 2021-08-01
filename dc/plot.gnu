set terminal pngcairo enhanced color dashed font "Alegreya, 14" \
rounded size 16 cm, 9.6 cm

# Default encoding, line styles, pallette, border and grid are set in
# /usr/local/share/gnuplot/x.y/gnuplotrc.

load '/labhome/denisn/.gnuplot'
set xlabel "Pollers"
set ylabel "Memory, MB"
set grid
set key left top
set xrange[0:25]

set yrange[100:1500]
set output './imgs/mem_hwm_many.png'
set title 'HWM RC vs DC'
plot 'RC_Many/rc_result.txt' u 1:4 t 'RC' w l ls 1, \
     'DC_Many/dc_result.txt' u 1:4 t 'DC' w l ls 2, \

set yrange[800:3800]
set output './imgs/mem_huge_many.png'
set title 'Huge pages RC vs DC'
plot 'RC_Many/rc_result.txt' u 1:3 t 'RC' w p ls 1, \
     'DC_Many/dc_result.txt' u 1:3 t 'DC' w p ls 2, \
