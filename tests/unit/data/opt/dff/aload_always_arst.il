# hand-written for Task 11 (read unedited by Yosys 0.55 and by proc_build): always-active ALOAD on $adlatch, both ARST polarities
autoidx 1
module \top
  wire width 2 input 1 \AD
  wire input 2 \ARST
  wire width 4 output 3 \Q
  cell $adlatch \ff0
    parameter \EN_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 2'10
    parameter \WIDTH 2
    connect \ARST \ARST
    connect \D \AD
    connect \EN 1'1
    connect \Q \Q [1:0]
  end
  cell $adlatch \ff1
    parameter \EN_POLARITY 0
    parameter \ARST_POLARITY 0
    parameter \ARST_VALUE 2'01
    parameter \WIDTH 2
    connect \ARST \ARST
    connect \D \AD
    connect \EN 1'0
    connect \Q \Q [3:2]
  end
end
