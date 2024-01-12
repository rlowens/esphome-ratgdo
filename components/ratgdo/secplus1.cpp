
#include "ratgdo.h"
#include "secplus1.h"

#include "esphome/core/log.h"
#include "esphome/core/scheduler.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace ratgdo {
namespace secplus1 {

    static const char* const TAG = "ratgdo_secplus1";

    void Secplus1::setup(RATGDOComponent* ratgdo, Scheduler* scheduler, InternalGPIOPin* rx_pin, InternalGPIOPin* tx_pin)
    {
        this->ratgdo_ = ratgdo;
        this->scheduler_ = scheduler;
        this->tx_pin_ = tx_pin;
        this->rx_pin_ = rx_pin;

        this->sw_serial_.begin(1200, SWSERIAL_8E1, rx_pin->get_pin(), tx_pin->get_pin(), true);
        // this->sw_serial_.enableIntTx(false);
        // this->sw_serial_.enableAutoBaud(true);
    }


    void Secplus1::loop() 
    {
        auto cmd = this->read_command();
        if (cmd) {
            this->handle_command(cmd.value());
        }
    }

    void Secplus1::dump_config()
    {
        ESP_LOGCONFIG(TAG, "  Protocol: SEC+ v1");
    }


    void Secplus1::sync()
    {
        this->wall_panel_emulation_state_ = WallPanelEmulationState::WAITING;
        wall_panel_emulation_start_ = millis();
        this->scheduler_->cancel_timeout(this->ratgdo_, "wall_panel_emulation");
        this->wall_panel_emulation();

        this->scheduler_->set_timeout(this->ratgdo_, "", 45000, [=] {
            if (this->door_state == DoorState::UNKNOWN) {
                ESP_LOGW(TAG, "Triggering sync failed actions.");
                this->ratgdo_->sync_failed = true;
            }
        });
    }

    void Secplus1::wall_panel_emulation(size_t index)
    {
        if (this->wall_panel_emulation_state_ == WallPanelEmulationState::WAITING) {
            ESP_LOG1(TAG, "Looking for security+ 1.0 wall panel...");

            if (this->door_state != DoorState::UNKNOWN || this->light_state != LightState::UNKNOWN) {
                ESP_LOG1(TAG, "Wall panel detected");
                return;
            }
            if (millis() - wall_panel_emulation_start_ > 35000 && !this->wall_panel_starting_) {
                ESP_LOG1(TAG, "No wall panel detected. Switching to emulation mode.");
                this->wall_panel_emulation_state_ = WallPanelEmulationState::RUNNING;
            }
            this->scheduler_->set_timeout(this->ratgdo_, "wall_panel_emulation", 2000, [=] {
                this->wall_panel_emulation();
            });
            return;
        } else if (this->wall_panel_emulation_state_ == WallPanelEmulationState::RUNNING) {
            // ESP_LOG2(TAG, "[Wall panel emulation] Sending byte: [%02X]", secplus1_states[index]);
            this->sw_serial_.write(&secplus1_states[index], 1);
            index += 1;
            if (index == 18) {
                index = 15;
            }
            this->scheduler_->set_timeout(this->ratgdo_, "wall_panel_emulation", 250, [=] {
                this->wall_panel_emulation(index);
            });
        }
    }

    void Secplus1::light_action(LightAction action)
    {
        ESP_LOG1(TAG, "Light action: %s", LightAction_to_string(action));
        if (action == LightAction::UNKNOWN) {
            return;
        }
        if (action == LightAction::TOGGLE || 
            (action == LightAction::ON && this->light_state == LightState::OFF) || 
            (action == LightAction::OFF && this->light_state == LightState::ON)) {
            this->transmit_packet(toggle_light);
        }
    }

    void Secplus1::lock_action(LockAction action)
    {
        ESP_LOG1(TAG, "Lock action: %s", LockAction_to_string(action));
        if (action == LockAction::UNKNOWN) {
            return;
        }
        if (action == LockAction::TOGGLE || 
            (action == LockAction::LOCK && this->lock_state == LockState::UNLOCKED) || 
            (action == LockAction::UNLOCK && this->lock_state == LockState::LOCKED)) {
            this->transmit_packet(toggle_lock);
        }
    }

    void Secplus1::door_action(DoorAction action)
    {
        ESP_LOG1(TAG, "Door action: %s, door state: %s", DoorAction_to_string(action), DoorState_to_string(this->door_state));
        if (action == DoorAction::UNKNOWN) {
            return;
        }

        const uint32_t double_toggle_delay = 1000;
        if (action == DoorAction::TOGGLE) {
            this->transmit_packet(toggle_door);
        } else if (action == DoorAction::OPEN) {
            if (this->door_state == DoorState::CLOSED || this->door_state == DoorState::CLOSING) {
                this->transmit_packet(toggle_door);
            } else if (this->door_state == DoorState::STOPPED) {
                this->transmit_packet(toggle_door); // this starts closing door
                // this changes direction of door
                this->scheduler_->set_timeout(this->ratgdo_, "", double_toggle_delay, [=] {
                    this->transmit_packet(toggle_door);
                });
            }
        } else if (action == DoorAction::CLOSE) {
            if (this->door_state == DoorState::OPEN) {
                this->transmit_packet(toggle_door);
            } else if (this->door_state == DoorState::OPENING) {
                this->transmit_packet(toggle_door); // this switches to stopped
                // another toggle needed to close
                this->scheduler_->set_timeout(this->ratgdo_, "", double_toggle_delay, [=] {
                    this->transmit_packet(toggle_door);
                });
            } else if (this->door_state == DoorState::STOPPED) {
                this->transmit_packet(toggle_door);
            }
        } else if (action == DoorAction::STOP) {
            if (this->door_state == DoorState::OPENING) {
                this->transmit_packet(toggle_door);
            } else if (this->door_state == DoorState::CLOSING) {
                this->transmit_packet(toggle_door); // this switches to opening
                // another toggle needed to stop
                this->scheduler_->set_timeout(this->ratgdo_, "", double_toggle_delay, [=] {
                    this->transmit_packet(toggle_door);
                });
            }
        }
    }


    Result Secplus1::call(Args args) 
    {
        return {};
    }

    optional<Command> Secplus1::read_command() 
    {
        static bool reading_msg = false;
        static uint32_t msg_start = 0;
        static uint16_t byte_count = 0;
        static RxPacket rx_packet;

        if (!reading_msg) {
            while (this->sw_serial_.available()) {
                uint8_t ser_byte = this->sw_serial_.read();
                this->last_rx_ = millis();

                if(ser_byte < 0x30 || ser_byte > 0x3A){
                    ESP_LOG2(TAG, "[%d] Ignoring byte [%02X], baud: %d", millis(), ser_byte, this->sw_serial_.baudRate());
                    byte_count = 0;
                    continue;
                }
                rx_packet[byte_count++] = ser_byte;
                reading_msg = true;
                break;
            }
        }
        if (reading_msg) {
            while (this->sw_serial_.available()) {
                uint8_t ser_byte = this->sw_serial_.read();
                this->last_rx_ = millis();
                rx_packet[byte_count++] = ser_byte;

                if (byte_count == RX_LENGTH) {
                    reading_msg = false;
                    byte_count = 0;
                    this->print_rx_packet(rx_packet);
                    return this->decode_packet(rx_packet);
                }
            }

            if (millis() - this->last_rx_ > 100) {
                // if we have a partial packet and it's been over 100ms since last byte was read,
                // the rest is not coming (a full packet should be received in ~20ms),
                // discard it so we can read the following packet correctly
                ESP_LOGW(TAG, "[%d] Discard incomplete packet, [%02X ...]", millis(), rx_packet[0]);
                reading_msg = false;
                byte_count = 0;
            }
        }
        
        return {};
    }

    void Secplus1::print_rx_packet(const RxPacket& packet) const
    {
        ESP_LOG2(TAG, "[%d] Received packet: [%02X %02X]", millis(), packet[0], packet[1]);
    }

    void Secplus1::print_tx_packet(const TxPacket& packet) const
    {
        ESP_LOG2(TAG, "[%d] Sending packet: [%02X %02X]", millis(), packet[0], packet[1]);
    }


    optional<Command> Secplus1::decode_packet(const RxPacket& packet) const
    {
        CommandType cmd_type = to_CommandType(packet[0], CommandType::UNKNOWN);
        return Command{cmd_type, packet[1]};
    }


    void Secplus1::handle_command(const Command& cmd)
    {
        if (cmd.type == CommandType::DOOR_STATUS) {

            DoorState door_state;
            auto val = cmd.value & 0x7;
            // 000 0x0 stopped
            // 001 0x1 opening
            // 010 0x2 open
            // 100 0x4 closing
            // 101 0x5 closed
            // 110 0x6 stopped

            if (val == 0x2){
			    door_state = DoorState::OPEN;
            } else if (val == 0x5){
                door_state = DoorState::CLOSED;
            } else if (val == 0x0 || val == 0x6){
                door_state = DoorState::STOPPED;
            } else if (val == 0x1){
                door_state = DoorState::OPENING;
            } else if(val == 0x4){
                door_state = DoorState::CLOSING;
            } else{
                door_state = DoorState::UNKNOWN;
            }

            if (this->door_state != door_state) {
                this->prev_door_state = this->door_state;
                this->door_state = door_state;
            } else {
                this->ratgdo_->received(door_state);
            }
        }
        else if (cmd.type == CommandType::OTHER_STATUS) {
            LightState light_state = to_LightState((cmd.value >> 2) & 1, LightState::UNKNOWN);
            if (this->light_state != light_state) {
                this->light_state = light_state;
            } else {
                this->ratgdo_->received(light_state);
            }

            LockState lock_state = to_LockState((~cmd.value >> 3) & 1, LockState::UNKNOWN);
            if (this->lock_state != lock_state) {
                this->lock_state = lock_state;
            } else {
                this->ratgdo_->received(lock_state);
            }
        }
        else if (cmd.type == CommandType::OBSTRUCTION) {
            ObstructionState obstruction_state = cmd.value == 0 ? ObstructionState::CLEAR : ObstructionState::OBSTRUCTED;
            this->ratgdo_->received(obstruction_state);
        }
        else if (cmd.type == CommandType::WALL_PANEL_STARTING) {
            if (cmd.value == 0x31) {
                this->wall_panel_starting_ = true;
            }
        }
    }

    void Secplus1::transmit_packet(const TxPacket& packet)
    {
        this->print_tx_packet(packet);

        int32_t tx_delay = static_cast<int32_t>(this->last_rx_ + 125) - millis();
        while (tx_delay<0) {
            tx_delay += 250;
        }

        this->scheduler_->set_timeout(this->ratgdo_, "", tx_delay, [=] {
            this->sw_serial_.enableIntTx(false);
            this->sw_serial_.write(packet[0]);
            this->sw_serial_.enableIntTx(true);
        });
        this->scheduler_->set_timeout(this->ratgdo_, "", tx_delay+250, [=] {
            this->sw_serial_.enableIntTx(false);
            this->sw_serial_.write(packet[1]);
            this->sw_serial_.enableIntTx(true);
        });
        this->scheduler_->set_timeout(this->ratgdo_, "", tx_delay+290, [=] {
            this->sw_serial_.enableIntTx(false);
            this->sw_serial_.write(packet[1]);
            this->sw_serial_.enableIntTx(true);
        });


    }

} // namespace secplus1
} // namespace ratgdo
} // namespace esphome