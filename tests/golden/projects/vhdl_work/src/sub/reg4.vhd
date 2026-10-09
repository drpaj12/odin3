-- 4-bit register with synchronous reset.
library ieee;
use ieee.std_logic_1164.all;

entity reg4 is
    port (clk, rst : in std_logic;
          d        : in std_logic_vector(3 downto 0);
          q        : out std_logic_vector(3 downto 0));
end entity reg4;

architecture rtl of reg4 is
begin
    process (clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                q <= (others => '0');
            else
                q <= d;
            end if;
        end if;
    end process;
end architecture rtl;
