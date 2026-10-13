attribute \top 1
module \t
  wire input 1 \c1
  wire input 2 \c2
  wire width 4 input 3 \d
  wire width 4 output 4 \a
  wire width 4 output 5 \b
  cell $dff $c8
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \c1
    connect \D \d
    connect \Q \a
  end
  cell $dff $c9
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    connect \CLK \c2
    connect \D \a
    connect \Q \b
  end
end
