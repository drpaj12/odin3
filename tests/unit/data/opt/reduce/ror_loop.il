module \top
  wire input 1 \a
  wire input 2 \b
  wire output 3 \x
  wire output 4 \y
  cell $reduce_or \rx
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \y \a }
    connect \Y \x
  end
  cell $reduce_or \ry
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \x \b }
    connect \Y \y
  end
end
