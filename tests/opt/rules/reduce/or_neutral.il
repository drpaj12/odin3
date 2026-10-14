attribute \top 1
module \t
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y
  cell $reduce_or $c6
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \b \a }
    connect \Y \y
  end
end
