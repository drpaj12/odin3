attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \r
  wire output 4 \q
  wire $n5
  attribute \full_case 1
  cell $mux $c8
    parameter \WIDTH 1
    connect \A \d
    connect \B 1'0
    connect \S \r
    connect \Y $n5
  end
  cell $sdff $c10
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \SRST_POLARITY 1
    parameter \SRST_VALUE 1'0
    connect \CLK \clk
    connect \SRST \r
    connect \D \d
    connect \Q \q
  end
end
