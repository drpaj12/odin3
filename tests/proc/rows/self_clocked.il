attribute \top 1
module \t
  wire output 1 \a
  wire $n2
  cell $not $c2
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y $n2
  end
  cell $dff $c4
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \a
    connect \D $n2
    connect \Q \a
  end
end
