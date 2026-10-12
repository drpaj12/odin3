module \top
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y
  cell $reduce_or \r
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A { 1'0 \b \a \b }
    connect \Y \y
  end
end
