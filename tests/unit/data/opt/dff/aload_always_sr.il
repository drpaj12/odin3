# hand-written for Task 11 (read unedited by Yosys 0.55 and by proc_build): always-active ALOAD on $dlatchsr, both SET/CLR polarity pairs
autoidx 1
module \top
  wire width 2 input 1 \AD
  wire width 2 input 2 \SET
  wire width 2 input 3 \CLR
  wire width 4 output 4 \Q
  cell $dlatchsr \ff0
    parameter \EN_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 0
    parameter \WIDTH 2
    connect \CLR \CLR
    connect \D \AD
    connect \EN 1'1
    connect \Q \Q [1:0]
    connect \SET \SET
  end
  cell $dlatchsr \ff1
    parameter \EN_POLARITY 0
    parameter \SET_POLARITY 0
    parameter \CLR_POLARITY 1
    parameter \WIDTH 2
    connect \CLR \CLR
    connect \D \AD
    connect \EN 1'0
    connect \Q \Q [3:2]
    connect \SET \SET
  end
end
