set terminal pngcairo enhanced color dashed font "Alegreya, 14" \
rounded size 16 cm, 9.6 cm

# Default encoding, line styles, pallette, border and grid are set in
# /usr/local/share/gnuplot/x.y/gnuplotrc.

load '/labhome/denisn/.gnuplot'
set xlabel "Connections"
set ylabel "Memory, MB"
set grid
set key right top
set xrange[0:15]

set yrange[100:1000]
set output './imgs/mem_many_hwm.png'
set title 'HWM RC vs DC'
plot 'RC_MANY/report.txt' u 1:4 every 2 t 'RC' w p ls 1, \
     'RC_MANY/report.txt' u 1:4 every 2::1 t 'RC 2x' w p ls 3

#     'DC_MANY/report.txt' u 2:4 every 2 t 'DC' w l ls 2, \
     'RC_MANY/report.txt' u 2:4 every 2::1 t 'RC 2x' w l ls 3, \
#     'DC_MANY/report.txt' u 2:4 every 2::1 t 'DC 2x' w l ls 4, \


# set yrange[880:950]
# set output 'mem_huge.png'
# set title 'Huge pages RC vs DC'
# plot 'RC_TGT_1_MANY_PERFS/results_rc.txt' u 2:3 t 'RC' w l ls 1, \
#      'DC_TGT_1_MANY_PERFS/results_dc.txt' u 2:3 t 'DC' w l ls 2, \
