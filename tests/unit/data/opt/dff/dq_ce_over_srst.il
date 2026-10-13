# hand-written for Task 11 (read unedited by Yosys 0.55 and by proc_build): D = Q on FFs with a sync reset, EN over SRST in every polarity, and SRST over EN
autoidx 1
module \top
  wire input 1 \CLK
  wire input 2 \EN
  wire input 3 \SRST
  wire width 4 output 4 \Q
  cell $sdffce \ff0
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    parameter \SRST_POLARITY 1
    parameter \SRST_VALUE 1'1
    parameter \WIDTH 1
    connect \CLK \CLK
    connect \D \Q [0]
    connect \EN \EN
    connect \Q \Q [0]
    connect \SRST \SRST
  end
  cell $sdffce \ff1
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 0
    parameter \SRST_POLARITY 0
    parameter \SRST_VALUE 1'0
    parameter \WIDTH 1
    connect \CLK \CLK
    connect \D \Q [1]
    connect \EN \EN
    connect \Q \Q [1]
    connect \SRST \SRST
  end
  cell $sdffe \ff2
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 0
    parameter \SRST_POLARITY 0
    parameter \SRST_VALUE 1'0
    parameter \WIDTH 1
    connect \CLK \CLK
    connect \D \Q [2]
    connect \EN \EN
    connect \Q \Q [2]
    connect \SRST \SRST
  end
  cell $sdff \ff3
    parameter \CLK_POLARITY 1
    parameter \SRST_POLARITY 0
    parameter \SRST_VALUE 1'1
    parameter \WIDTH 1
    connect \CLK \CLK
    connect \D \Q [3]
    connect \Q \Q [3]
    connect \SRST \SRST
  end
end
