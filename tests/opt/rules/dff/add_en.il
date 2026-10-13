attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \en
  wire output 4 \q
  wire $n5
  cell $mux $c7
    parameter \WIDTH 1
    connect \A \q
    connect \B \d
    connect \S \en
    connect \Y $n5
  end
  cell $dffe $c9
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
