attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \en
  wire width 4 input 3 \d
  wire width 4 input 4 \e
  wire width 4 output 5 \a
  wire width 4 output 6 \b
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \a
    connect \B \d
    connect \S \en
    connect \Y { $n22 $n21 $n20 $n19 }
  end
  cell $dff $c10
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n22 $n21 $n20 $n19 }
    connect \Q \a
  end
  cell $dff $c11
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \e
    connect \Q \b
  end
end
