module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire width 2 \t
  wire output 4 \y
  cell $reduce_or \r1
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A { \a \b }
    connect \Y \t
  end
  cell $reduce_or \r2
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \t [1] \c }
    connect \Y \y
  end
end
