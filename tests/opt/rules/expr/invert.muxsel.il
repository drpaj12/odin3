attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire output 4 \y
  wire $n5
  cell $not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \s
    connect \Y $n5
  end
  cell $mux $c7
    parameter \WIDTH 1
    connect \A \a
    connect \B \b
    connect \S \s
    connect \Y \y
  end
end
